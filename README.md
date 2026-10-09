# OBS Dock Layout

An OBS Studio plugin that adds **Tools → Dock Layout**. It can save named layouts, capture the OBS main-window geometry and Qt dock state, associate layouts with a display, and apply the matching profile after the OBS window moves to another monitor.

## Build output

The Windows x64 build is produced by GitHub Actions using the official OBS plugin-template build setup. Successful builds are published under the repository's **Releases** page:

https://github.com/SiczenEXT/Dock-Layout/releases

The release includes `obs-dock-layout.dll` and a ZIP arranged as `obs-plugins/64bit/obs-dock-layout.dll`.

## Important compatibility note

The automated Windows build targets the OBS 32.2.0 SDK with the matching OBS/Qt dependency bundle, intended for stable OBS 32.2.x. A successful compile does not replace testing the plugin in your own OBS installation; dock-state restoration and monitor switching can vary with OBS/Qt versions.

## Usage

1. Install the DLL into your OBS plugin directory, typically under `obs-plugins/64bit`.
2. Restart OBS Studio.
3. Open **Tools → Dock Layout**.
4. Enter a name, select a display, arrange your docks, and choose **Save Current Layout**.
5. Save a separate profile for each monitor. Use **Apply Selected** to restore one manually.

The plugin stores profiles in OBS's plugin configuration directory in `layouts.json`.

## Limitations

A profile captures the OBS main window's geometry and Qt dock state. It does not save independent layouts for separate floating dock windows. Screen matching uses display name/serial where possible and falls back to saved geometry; re-save profiles if Windows changes monitor identification or arrangement.
