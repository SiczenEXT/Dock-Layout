#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFont>
#include <QByteArray>
#include <QComboBox>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWindow>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QPointer>
#include <QCloseEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QResizeEvent>

#include <limits>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-dock-layout", "en-US")

namespace {
QPointer<QDialog> g_dialog;
QPointer<QAction> g_action;
QPointer<QMainWindow> g_mainWindow;
QTimer *g_screenPoll = nullptr;
bool g_applying = false;
QString g_configFile;
QString g_lastScreenKey;

struct LayoutProfile {
    QString name;
    QString screenName;
    QString screenSerial;
    QString displayKey; // Exact display identity for filtering, including duplicate-name monitors.
    QRect geometry;
    QByteArray dockState;
    QString windowMode; // Normal, Maximized, Fullscreen, Minimized
};


QString screenKey(QScreen *screen)
{
    if (!screen)
        return {};
    // Screen names are stable enough for typical desktop setups. Geometry is
    // included to distinguish displays with duplicated names.
    return QString("%1|%2,%3 %4x%5")
        .arg(screen->name())
        .arg(screen->geometry().x()).arg(screen->geometry().y())
        .arg(screen->geometry().width()).arg(screen->geometry().height());
}

QScreen *screenForKey(const QString &name, const QString &serial, const QRect &savedGeometry)
{
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens) {
        if (!serial.isEmpty() && s->serialNumber() == serial && s->name() == name)
            return s;
    }
    for (QScreen *s : screens) {
        if (s->name() == name)
            return s;
    }
    // Fall back to the screen whose center is closest to the saved window center.
    QScreen *best = QGuiApplication::primaryScreen();
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    const QPoint target = savedGeometry.center();
    for (QScreen *s : screens) {
        const QPoint c = s->geometry().center();
        const qint64 dx = qint64(c.x()) - target.x();
        const qint64 dy = qint64(c.y()) - target.y();
        const qint64 d = dx * dx + dy * dy;
        if (d < bestDistance) {
            bestDistance = d;
            best = s;
        }
    }
    return best;
}

QScreen *screenForProfile(const LayoutProfile &p)
{
    const auto screens = QGuiApplication::screens();

    // Match the monitor's hardware serial first. This keeps profiles assigned
    // correctly when resolution or monitor position changes.
    if (!p.screenSerial.isEmpty()) {
        for (QScreen *s : screens) {
            if (s->name() == p.screenName && s->serialNumber() == p.screenSerial)
                return s;
        }
    }

    // Exact geometry-based identity distinguishes same-model monitors when a
    // serial number is unavailable.
    if (!p.displayKey.isEmpty()) {
        for (QScreen *s : screens)
            if (screenKey(s) == p.displayKey)
                return s;
    }

    // Older profiles may not have an exact display key. Use a name only when
    // that name identifies exactly one attached screen; avoid guessing between
    // two identical monitors or moving a disconnected screen's profile.
    QScreen *nameMatch = nullptr;
    int nameMatches = 0;
    for (QScreen *s : screens) {
        if (s->name() == p.screenName) {
            nameMatch = s;
            ++nameMatches;
        }
    }
    if (nameMatches == 1)
        return nameMatch;

    // Legacy fallback only for a profile with no recorded display identity.
    if (p.displayKey.isEmpty() && p.screenSerial.isEmpty() && p.screenName.isEmpty())
        return screenForKey(p.screenName, p.screenSerial, p.geometry);

    return nullptr;
}

QJsonObject profileToJson(const LayoutProfile &p)
{
    QJsonObject o;
    o["name"] = p.name;
    o["screenName"] = p.screenName;
    o["screenSerial"] = p.screenSerial;
    o["displayKey"] = p.displayKey;
    o["x"] = p.geometry.x();
    o["y"] = p.geometry.y();
    o["width"] = p.geometry.width();
    o["height"] = p.geometry.height();
    o["dockState"] = QString::fromLatin1(p.dockState.toBase64());
    o["windowMode"] = p.windowMode;
    return o;
}

LayoutProfile profileFromJson(const QJsonObject &o)
{
    LayoutProfile p;
    p.name = o["name"].toString();
    p.screenName = o["screenName"].toString();
    p.screenSerial = o["screenSerial"].toString();
    p.displayKey = o["displayKey"].toString();
    p.geometry = QRect(o["x"].toInt(), o["y"].toInt(),
                       o["width"].toInt(1200), o["height"].toInt(800));
    p.dockState = QByteArray::fromBase64(o["dockState"].toString().toLatin1());
    p.windowMode = o["windowMode"].toString("Normal");
    return p;
}

QList<LayoutProfile> loadProfiles()
{
    QList<LayoutProfile> out;
    QFile f(g_configFile);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    for (const auto &v : doc.array())
        if (v.isObject())
            out.append(profileFromJson(v.toObject()));
    return out;
}

void saveProfiles(const QList<LayoutProfile> &items)
{
    QJsonArray arr;
    for (const auto &p : items)
        arr.append(profileToJson(p));
    QDir().mkpath(QFileInfo(g_configFile).absolutePath());
    QFile f(g_configFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

QList<LayoutProfile> currentProfiles()
{
    return loadProfiles();
}

void replaceProfiles(const QList<LayoutProfile> &items)
{
    saveProfiles(items);
}

void applyProfile(const LayoutProfile &p)
{
    if (!g_mainWindow || g_applying)
        return;

    g_applying = true;
    QMainWindow *mw = g_mainWindow;
    // Restore the saved dock layout before geometry/window state.
    if (!p.dockState.isEmpty())
        mw->restoreState(p.dockState, 1);

    QScreen *target = screenForProfile(p);
    QRect targetGeometry = p.geometry;
    if (target) {
        // Preserve saved window size and place it within the selected display.
        const QRect available = target->availableGeometry();
        if (!targetGeometry.isValid())
            targetGeometry = QRect(available.topLeft(), QSize(1200, 800));
        if (!available.intersects(targetGeometry)) {
            targetGeometry.moveTopLeft(available.topLeft());
        } else {
            if (targetGeometry.right() > available.right())
                targetGeometry.moveRight(available.right());
            if (targetGeometry.bottom() > available.bottom())
                targetGeometry.moveBottom(available.bottom());
            if (targetGeometry.left() < available.left())
                targetGeometry.moveLeft(available.left());
            if (targetGeometry.top() < available.top())
                targetGeometry.moveTop(available.top());
        }
    }
    mw->showNormal();
    mw->setGeometry(targetGeometry);

    if (p.windowMode == "Fullscreen")
        mw->showFullScreen();
    else if (p.windowMode == "Maximized")
        mw->showMaximized();
    else if (p.windowMode == "Minimized")
        mw->showMinimized();
    else
        mw->showNormal();

    g_lastScreenKey = screenKey(mw->screen());
    g_applying = false;
}

void switchForCurrentScreen()
{
    if (!g_mainWindow || g_applying)
        return;
    QScreen *screen = g_mainWindow->screen();
    if (!screen && g_mainWindow->windowHandle())
        screen = g_mainWindow->windowHandle()->screen();
    const QString key = screenKey(screen);
    if (key.isEmpty() || key == g_lastScreenKey)
        return;

    // A short delay lets Windows/Qt settle after the window crosses displays.
    QTimer::singleShot(350, [key]() {
        if (!g_mainWindow || g_applying)
            return;
        QScreen *current = g_mainWindow->screen();
        const QString actualKey = screenKey(current);
        if (actualKey.isEmpty() || actualKey != key)
            return;
        g_lastScreenKey = actualKey;
        const auto items = currentProfiles();
        for (const auto &p : items) {
            QScreen *savedScreen = screenForProfile(p);
            if (savedScreen && screenKey(savedScreen) == actualKey) {
                applyProfile(p);
                return;
            }
        }
    });
}

class LayoutDialog final : public QDialog {
public:
    explicit LayoutDialog(QWidget *parent) : QDialog(parent)
    {
        setWindowTitle("Dock Layout");
        setMinimumSize(520, 470);
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        // Inherit OBS's current palette and stylesheet rather than forcing a
        // separate dark theme. This follows the selected OBS theme.
        if (parent) {
            setPalette(parent->palette());
            setFont(parent->font());
        } else {
            setPalette(QApplication::palette());
            setFont(QApplication::font());
        }

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(10);

        auto *header = new QLabel("Dock Layout", this);
        QFont headerFont = header->font();
        headerFont.setBold(true);
        headerFont.setPointSize(headerFont.pointSize() + 3);
        header->setFont(headerFont);
        const QColor accent = palette().color(QPalette::Highlight);
        const QColor accentText = palette().color(QPalette::HighlightedText);
        header->setStyleSheet(QString(
            "QLabel { background-color: %1; color: %2; padding: 10px 12px; border-radius: 4px; }")
            .arg(accent.name(), accentText.name()));
        root->addWidget(header);

        auto *hint = new QLabel("Save and restore OBS workspace layouts for each monitor.", this);
        hint->setWordWrap(true);
        root->addWidget(hint);

        root->addWidget(new QLabel("Layout name", this));
        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText("e.g. Streaming Monitor");
        root->addWidget(nameEdit);

        root->addWidget(new QLabel("Select display"));
        displayCombo = new QComboBox(this);
        int currentDisplayIndex = -1;
        QScreen *currentScreen = g_mainWindow ? g_mainWindow->screen() : nullptr;
        for (QScreen *s : QGuiApplication::screens()) {
            displayCombo->addItem(QString("%1 — %2×%3 at %4,%5")
                .arg(s->name()).arg(s->geometry().width()).arg(s->geometry().height())
                .arg(s->geometry().x()).arg(s->geometry().y()), screenKey(s));
            if (currentScreen && screenKey(s) == screenKey(currentScreen))
                currentDisplayIndex = displayCombo->count() - 1;
        }
        if (currentDisplayIndex >= 0)
            displayCombo->setCurrentIndex(currentDisplayIndex);
        root->addWidget(displayCombo);

        root->addWidget(new QLabel("Window state to apply when this layout activates"));
        modeCombo = new QComboBox(this);
        modeCombo->addItems({"Normal", "Maximized", "Fullscreen", "Minimized"});
        root->addWidget(modeCombo);

        root->addWidget(new QLabel("Saved layouts"));
        layoutList = new QListWidget(this);
        root->addWidget(layoutList, 1);

        auto *orderRow = new QHBoxLayout;
        auto *up = new QPushButton("Move Up", this);
        auto *down = new QPushButton("Move Down", this);
        auto *del = new QPushButton("Delete", this);
        orderRow->addWidget(up);
        orderRow->addWidget(down);
        orderRow->addWidget(del);
        root->addLayout(orderRow);

        auto *bottom = new QHBoxLayout;
        auto *save = new QPushButton("Save Current Layout", this);
        auto *apply = new QPushButton("Apply Selected", this);
        auto *close = new QPushButton("Close", this);
        save->setDefault(true);
        bottom->addWidget(save);
        bottom->addWidget(apply);
        bottom->addStretch();
        bottom->addWidget(close);
        root->addLayout(bottom);

        connect(displayCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { refreshList(); });
        connect(save, &QPushButton::clicked, this, [this]() { saveCurrent(); });
        connect(apply, &QPushButton::clicked, this, [this]() { applySelected(); });
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        connect(del, &QPushButton::clicked, this, [this]() { deleteSelected(); });
        connect(up, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
        connect(down, &QPushButton::clicked, this, [this]() { moveSelected(1); });
        connect(layoutList, &QListWidget::currentRowChanged,
                this, [this](int row) { updateFieldsForRow(row); });
        refreshList();
    }

private:
    QLineEdit *nameEdit = nullptr;
    QComboBox *displayCombo = nullptr;
    QComboBox *modeCombo = nullptr;
    QListWidget *layoutList = nullptr;
    QList<int> visibleProfileIndices;

    QString selectedScreenKey() const
    {
        return displayCombo && displayCombo->currentIndex() >= 0
            ? displayCombo->currentData().toString() : QString();
    }

    void updateFieldsForRow(int row)
    {
        if (row < 0 || row >= visibleProfileIndices.size()) {
            nameEdit->clear();
            return;
        }
        const auto items = currentProfiles();
        const int profileIndex = visibleProfileIndices[row];
        if (profileIndex < 0 || profileIndex >= items.size()) {
            nameEdit->clear();
            return;
        }
        nameEdit->setText(items[profileIndex].name);
        modeCombo->setCurrentText(items[profileIndex].windowMode);
    }

    void refreshList(int selected = -1)
    {
        const auto items = currentProfiles();
        const QString wantedScreen = selectedScreenKey();
        visibleProfileIndices.clear();

        // Keep all profiles on disk; filter only the visible list by monitor.
        for (int i = 0; i < items.size(); ++i) {
            const auto &p = items[i];
            QScreen *savedScreen = screenForProfile(p);
            if (savedScreen && screenKey(savedScreen) == wantedScreen)
                visibleProfileIndices.append(i);
        }

        const QSignalBlocker blocker(layoutList);
        layoutList->clear();
        for (int profileIndex : visibleProfileIndices)
            layoutList->addItem(items[profileIndex].name);

        if (!visibleProfileIndices.isEmpty()) {
            const int row = selected < 0
                ? 0 : qBound(0, selected, visibleProfileIndices.size() - 1);
            layoutList->setCurrentRow(row);
            updateFieldsForRow(row);
        } else {
            nameEdit->clear();
        }
    }

    void saveCurrent()
    {
        if (!g_mainWindow) return;
        const QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::information(this, "Dock Layout", "Enter a layout name first.");
            return;
        }
        QScreen *screen = nullptr;
        if (displayCombo->currentIndex() >= 0) {
            const QString wanted = displayCombo->currentData().toString();
            for (QScreen *s : QGuiApplication::screens())
                if (screenKey(s) == wanted) { screen = s; break; }
        }
        if (!screen) screen = g_mainWindow->screen();
        if (!screen) screen = QGuiApplication::primaryScreen();

        LayoutProfile p;
        p.name = name;
        p.screenName = screen ? screen->name() : QString();
        p.screenSerial = screen ? screen->serialNumber() : QString();
        p.displayKey = screen ? screenKey(screen) : QString();
        p.geometry = g_mainWindow->geometry();
        p.dockState = g_mainWindow->saveState(1);
        p.windowMode = modeCombo->currentText();

        auto items = currentProfiles();
        int index = -1;
        const QString wantedScreen = screen ? screenKey(screen) : QString();
        for (int i = 0; i < items.size(); ++i) {
            QScreen *existingScreen = screenForProfile(items[i]);
            const bool sameDisplay = existingScreen &&
                screenKey(existingScreen) == wantedScreen;
            if (items[i].name == name && sameDisplay) {
                index = i;
                break;
            }
        }
        if (index >= 0)
            items[index] = p;
        else {
            items.append(p);
            index = items.size() - 1;
        }
        replaceProfiles(items);
        refreshList();
        const int visibleRow = visibleProfileIndices.indexOf(index);
        if (visibleRow >= 0) {
            layoutList->setCurrentRow(visibleRow);
            updateFieldsForRow(visibleRow);
        }
    }

    void applySelected()
    {
        const int row = layoutList->currentRow();
        const auto items = currentProfiles();
        if (row < 0 || row >= visibleProfileIndices.size())
            return;
        const int profileIndex = visibleProfileIndices[row];
        if (profileIndex >= 0 && profileIndex < items.size())
            applyProfile(items[profileIndex]);
    }

    void deleteSelected()
    {
        const int row = layoutList->currentRow();
        auto items = currentProfiles();
        if (row < 0 || row >= visibleProfileIndices.size())
            return;
        const int profileIndex = visibleProfileIndices[row];
        if (profileIndex < 0 || profileIndex >= items.size())
            return;
        items.removeAt(profileIndex);
        replaceProfiles(items);
        refreshList(qMin(row, visibleProfileIndices.size() - 1));
    }

    void moveSelected(int delta)
    {
        const int row = layoutList->currentRow();
        auto items = currentProfiles();
        const int next = row + delta;
        if (row < 0 || next < 0 || next >= visibleProfileIndices.size())
            return;
        const int sourceIndex = visibleProfileIndices[row];
        const int targetIndex = visibleProfileIndices[next];
        if (sourceIndex < 0 || targetIndex < 0 ||
            sourceIndex >= items.size() || targetIndex >= items.size())
            return;
        items.swapItemsAt(sourceIndex, targetIndex);
        replaceProfiles(items);
        refreshList(next);
    }
};

void showDialog()
{
    if (g_dialog) {
        g_dialog->show();
        g_dialog->raise();
        g_dialog->activateWindow();
        return;
    }
    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    auto *dialog = new LayoutDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    g_dialog = dialog;
    dialog->show();
}

void frontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        QWidget *w = static_cast<QWidget *>(obs_frontend_get_main_window());
        g_mainWindow = qobject_cast<QMainWindow *>(w);
        if (!g_mainWindow)
            g_mainWindow = w ? w->findChild<QMainWindow *>() : nullptr;
        if (g_mainWindow) {
            g_lastScreenKey = screenKey(g_mainWindow->screen());
            QObject::connect(g_mainWindow, &QWidget::windowTitleChanged,
                             g_mainWindow, []() { switchForCurrentScreen(); });
            if (!g_screenPoll) {
                g_screenPoll = new QTimer(qApp);
                g_screenPoll->setInterval(400);
                QObject::connect(g_screenPoll, &QTimer::timeout, []() {
                    if (g_mainWindow && !g_applying)
                        switchForCurrentScreen();
                });
                g_screenPoll->start();
            }
        }
    }
}
} // namespace

bool obs_module_load(void)
{
    char *path = obs_module_config_path("layouts.json");
    if (path) {
        g_configFile = QString::fromUtf8(path);
        bfree(path);
    }

    auto *action = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction("Dock Layout"));
    g_action = action;
    if (action)
        QObject::connect(action, &QAction::triggered, []() { showDialog(); });
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    blog(LOG_INFO, "[obs-dock-layout] Loaded Dock Layout plugin");
    return true;
}

void obs_module_unload(void)
{
    if (g_screenPoll) {
        g_screenPoll->stop();
        delete g_screenPoll;
        g_screenPoll = nullptr;
    }
    if (g_action) {
        // OBS 32.x returns the Tools-menu QAction; deleting it removes it
        // from the menu and prevents callbacks into an unloaded plugin.
        delete g_action.data();
        g_action = nullptr;
    }
    if (g_dialog) {
        g_dialog->close();
        g_dialog = nullptr;
    }
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
