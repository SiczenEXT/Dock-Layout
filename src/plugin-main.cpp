#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QApplication>
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

QJsonObject profileToJson(const LayoutProfile &p)
{
    QJsonObject o;
    o["name"] = p.name;
    o["screenName"] = p.screenName;
    o["screenSerial"] = p.screenSerial;
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

    QScreen *target = screenForKey(p.screenName, p.screenSerial, p.geometry);
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
            QScreen *savedScreen = screenForKey(p.screenName, p.screenSerial, p.geometry);
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
        setStyleSheet(
            "QDialog { background:#202020; color:#eeeeee; }"
            "QLabel { color:#eeeeee; }"
            "QLineEdit,QComboBox,QListWidget { background:#303030; color:#eeeeee;"
            " border:1px solid #505050; padding:6px; }"
            "QPushButton { background:#3b3b3b; color:#f2f2f2; border:1px solid #555;"
            " border-radius:3px; padding:7px 12px; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton:pressed { background:#2d2d2d; }"
            "QListWidget::item:selected { background:#365b85; }"
        );

        auto *root = new QVBoxLayout(this);
        root->addWidget(new QLabel("Layout name"));
        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText("e.g. Streaming Monitor");
        root->addWidget(nameEdit);

        root->addWidget(new QLabel("Select display"));
        displayCombo = new QComboBox(this);
        for (QScreen *s : QGuiApplication::screens()) {
            displayCombo->addItem(QString("%1 — %2×%3 at %4,%5")
                .arg(s->name()).arg(s->geometry().width()).arg(s->geometry().height())
                .arg(s->geometry().x()).arg(s->geometry().y()), screenKey(s));
        }
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

        connect(save, &QPushButton::clicked, this, [this]() { saveCurrent(); });
        connect(apply, &QPushButton::clicked, this, [this]() { applySelected(); });
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        connect(del, &QPushButton::clicked, this, [this]() { deleteSelected(); });
        connect(up, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
        connect(down, &QPushButton::clicked, this, [this]() { moveSelected(1); });
        connect(layoutList, &QListWidget::currentRowChanged, this, [this](int row) {
            const auto items = currentProfiles();
            if (row >= 0 && row < items.size()) {
                nameEdit->setText(items[row].name);
                modeCombo->setCurrentText(items[row].windowMode);
                for (int i = 0; i < displayCombo->count(); ++i) {
                    QScreen *s = screenForKey(items[row].screenName, items[row].screenSerial,
                                              items[row].geometry);
                    if (s && displayCombo->itemData(i).toString() == screenKey(s)) {
                        displayCombo->setCurrentIndex(i);
                        break;
                    }
                }
            }
        });
        refreshList();
    }

private:
    QLineEdit *nameEdit = nullptr;
    QComboBox *displayCombo = nullptr;
    QComboBox *modeCombo = nullptr;
    QListWidget *layoutList = nullptr;

    void refreshList(int selected = -1)
    {
        const auto items = currentProfiles();
        layoutList->clear();
        for (const auto &p : items)
            layoutList->addItem(QString("%1   —   %2").arg(p.name, p.screenName));
        if (!items.isEmpty())
            layoutList->setCurrentRow(qBound(0, selected < 0 ? 0 : selected, items.size()-1));
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
        p.geometry = g_mainWindow->geometry();
        p.dockState = g_mainWindow->saveState(1);
        p.windowMode = modeCombo->currentText();

        auto items = currentProfiles();
        int index = -1;
        for (int i = 0; i < items.size(); ++i)
            if (items[i].name == name) { index = i; break; }
        if (index >= 0) items[index] = p;
        else items.append(p);
        replaceProfiles(items);
        refreshList(index >= 0 ? index : items.size() - 1);
    }

    void applySelected()
    {
        const int row = layoutList->currentRow();
        const auto items = currentProfiles();
        if (row >= 0 && row < items.size())
            applyProfile(items[row]);
    }

    void deleteSelected()
    {
        const int row = layoutList->currentRow();
        auto items = currentProfiles();
        if (row < 0 || row >= items.size()) return;
        items.removeAt(row);
        replaceProfiles(items);
        refreshList(qMin(row, items.size() - 1));
    }

    void moveSelected(int delta)
    {
        const int row = layoutList->currentRow();
        auto items = currentProfiles();
        const int next = row + delta;
        if (row < 0 || next < 0 || next >= items.size()) return;
        items.swapItemsAt(row, next);
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
