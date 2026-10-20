# Glassy

A tiny, dependency-free **round screen magnifier** for Windows with a *liquid glass* look: edge refraction, chromatic aberration, a specular rim and soft reflections. Written in plain C with the Win32 API only.

- Single `.c` file, no runtime or external libraries
- Smooth: separate threads for mouse hook, rendering and UI, synced to display refresh (60/144 Hz)
- Near-zero CPU when idle (nothing is redrawn if nothing changed)
- Lives in the system tray, remembers your settings

## Usage

| Action | Result |
| --- | --- |
| `Ctrl` + `Alt` + `Z` | Toggle the magnifier on/off |
| Mouse wheel *(while active)* | Zoom in / out (1.25x - 20x) |
| `Shift` + mouse wheel *(while active)* | Change lens size (150 - 900 px) |
| Tray icon, left click | Toggle on/off |
| Tray icon, right click | Menu: Settings, lens size presets, run at startup, exit |

### Settings

Open **Settings...** from the tray menu to configure:

- Lens size and zoom level
- Start automatically with Windows (enabled on first run)
- Zoom with the wheel only while `Ctrl` is held (otherwise the wheel is captured while the lens is active)
- Show / hide the lens in screenshots and screen sharing

Settings are stored per user in `HKEY_CURRENT_USER\Software\Glassy`. The startup entry lives in `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.

## Download

Grab the latest `Glassy.exe` from the [Releases](../../releases) page. No installer needed - just run it.

## Build from source

Requirements: Windows 10 or newer, and [MinGW-w64](https://www.mingw-w64.org/) (or MSYS2).

```bat
build.bat
```

or manually:

```bat
gcc -O2 -s -mwindows -municode Glassy.c -o Glassy.exe -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomctl32 -ldwmapi -lwinmm -lm
```

Cross-compiling from Linux/macOS:

```sh
make
```

## How it works

- **Hook thread** - installs a low-level mouse hook (`WH_MOUSE_LL`) only while the lens is active and does nothing except handle wheel events, so the mouse never lags.
- **Lens thread** - captures the area around the cursor, applies the liquid glass effect using precomputed lookup tables, and pushes the result with `UpdateLayeredWindow` (position and image in a single step). It waits on `DwmFlush` to stay in sync with the display and skips rendering when nothing has changed.
- **Main thread** - tray icon, global hotkey, on/off notification and the settings window.

## Notes

- The program installs a global low-level mouse hook while active. Some antivirus tools or Windows SmartScreen may warn about unsigned executables that do this; you can review the full source in this repository and build it yourself.
- "Hide lens from screenshots / screen sharing" uses `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, which requires Windows 10 version 2004 or newer.
- If `Ctrl+Alt+Z` is already taken by another application, Glassy shows a warning and the hotkey will not work (the tray icon still does).

## Contributing

Issues and pull requests are welcome. Please keep the project dependency-free (Win32 API + C standard library only).

## License

[MIT](LICENSE)
