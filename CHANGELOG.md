# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

## [2.0.0] - 2026-10-21

### Fixed
- Windows 10: the lens captured itself and showed lenses nested inside lenses. The lens is now always excluded from screen capture (`WDA_EXCLUDEFROMCAPTURE`), on every start and every time it is shown.

### Removed
- The "Show the lens in screenshots and streams" option and its `HideCapture` registry value. The lens is no longer visible in screenshots or recordings.

### Changed
- Windows 10 version 2004 (build 19041) or later is required. On older builds Glassy shows a warning because the lens cannot be hidden from capture there.

## [1.5.0] - 2026-10-21

First public release.

### Added
- Round lens that follows the cursor and is click-through.
- Liquid glass edge: refraction, chromatic aberration, specular rim, glint and inner shadow.
- Mouse wheel zoom (1.25x to 20x) and Shift + wheel lens resizing (150 to 900 px).
- Global hotkey `Ctrl+Alt+Z` and an on-screen ON/OFF notification.
- Tray icon with menu, quick lens size presets and a settings window.
- Optional start with Windows (enabled on first run).
- Option to zoom only while Ctrl is held, so normal scrolling keeps working.
- Persistent settings stored in `HKEY_CURRENT_USER\Software\Glassy`.
- Build scripts for MinGW-w64 and GitHub Actions workflows for CI and releases.

[2.0.0]: https://github.com/ridvanaslan0/Glassy/releases/tag/v2.0.1
[1.5.0]: https://github.com/ridvanaslan0/Glassy/releases/tag/v2.0.0
