# Porting

Everything except `src/platform` is portable C++26. `MappedFile`, `Paths`, `Process` and the IPC socket have Windows and
macOS code paths.

The portable part (core, dictionaries, lookup, `lexiglancectl`, the Qt settings application and the unit tests) builds
and passes its tests without any desktop libraries. Check it with the `portable` preset:

```sh
cmake --preset portable
cmake --build --preset portable
ctest --preset portable
```

The daemon additionally needs Cairo and Pango for the popup, libcurl for audio and Anki, and a desktop backend.

## Backends

A backend implements `lexiglance::platform::Backend` (`include/lexiglance/platform/Platform.h`) and is selected in
`src/platform/Platform.cpp`. Text capture is a `TextCapture`. OCR (`src/platform/ocr`) is shared by the backends: it
reads the screen through a `ScreenReader` each of them provides (`XGetImage` on X11, `BitBlt` on Windows), so a new
backend only brings its screenshot call (`CGWindowListCreateImage` on macOS).

| Piece               | X11 (done)                     | Windows (done)                                     | macOS (done)                                                                     | Wayland                                            |
| ------------------- | ------------------------------ | -------------------------------------------------- | -------------------------------------------------------------------------------- | -------------------------------------------------- |
| Trigger keys        | XInput2 raw events             | Raw Input with `RIDEV_INPUTSINK` (no hooks)        | `CGEventTap` (listen-only)                                                       | GlobalShortcuts portal                             |
| Text under pointer  | AT-SPI                         | UI Automation `TextPattern.RangeFromPoint`         | `AXUIElementCopyElementAtPosition` + `kAXRangeForPositionParameterizedAttribute` | AT-SPI (the pointer position needs the compositor) |
| Screen pixels (OCR) | `XGetImage` of the root window | `BitBlt` of the virtual screen                     | `CGWindowListCreateImage`                                                        | Screenshot portal                                  |
| Popup window        | override-redirect              | layered `WS_EX_NOACTIVATE \| WS_EX_TOPMOST` window | borderless non-activating `NSPanel`                                              | layer-shell surface                                |
| Highlight           | shaped, click-through window   | layered window with `WS_EX_TRANSPARENT`            | ignoring-mouse `NSWindow`                                                        | layer-shell surface                                |
| Selection lookup    | PRIMARY selection              | the clipboard                                      | the pasteboard                                                                   | primary selection protocol                         |
| Audio player        | ffplay, mpv, mpg123            | MCI (DirectShow), else ffplay or mpv               | afplay                                                                           | as X11                                             |
| IPC                 | Unix socket                    | named pipe, this user only                         | Unix socket                                                                      | Unix socket                                        |

On Wayland sessions the X11 backend runs through XWayland for X11 applications.

## Windows

`src/platform/windows` holds the backend: `WindowsBackend` (Raw Input, the popup and highlight windows, the clipboard,
the window under the pointer, the desktop's scale and colour mode), `UiaCapture` (text through UI Automation) and
`GdiScreen` (screen pixels for OCR). It runs on Windows 10 1809 or later and on Windows 11, like Qt 6.

### Building

Natively with [MSYS2](https://www.msys2.org) (UCRT64), which has GCC, Qt, Cairo, Pango and libcurl:

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,zlib,cairo,pango,curl,qt6-base}
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix dist   # the programs with the DLLs and Qt plugins they load
```

`cpack -G ZIP` in the build directory makes the `.zip`. The installer is built with NSIS from an install prefix:
`cmake --install build/release --prefix stage`, then `.github/scripts/windows-installer.ps1 build/release stage dist`.
`.github/scripts/smoke-test-windows.ps1 <package>` checks either with nothing but Windows on PATH (CI runs it on every
change). The MinGW-w64 cross build (`cmake/toolchains/mingw-w64.cmake`) covers the portable part, linked
statically and tested under wine.

### How it behaves

- Settings live in `%APPDATA%\Lexiglance\config`; dictionaries and OCR models, which are large, stay on the machine in
  `%LOCALAPPDATA%\Lexiglance\data`, and the log is in `%LOCALAPPDATA%\Lexiglance\state`.
- The daemon and the settings application open no console window; started from a terminal, they print to it.
- The default trigger, Win + Left Alt, never opens the Start menu. Letting go of Left Alt while Win is still held counts
  as a tap of Alt in the application below, which then shows its menu or key tips (Notepad on Windows 11); letting go
  of Win first does not. A passive backend cannot hide that tap, which would take a synthesised key.
- UI Automation reads the applications that implement its text pattern: Office and WordPad, browsers, Qt, WPF and WinUI
  applications, Windows Terminal. The classic Win32 edit control (Notepad on Windows 10) has none; OCR reads it.
- Selection lookup becomes lookup of copied text: Windows has no selection of its own, and text hookers copy what they
  read to the clipboard.
- The wheel changes the looked-up length while the trigger is held, and by default the window under the pointer scrolls
  too, since taking the wheel from it needs a low-level mouse hook. **Scanning -> Stop the window underneath from
  scrolling too** installs one while the trigger is held over a popup; it is off by default, and
  [anticheat.md](anticheat.md) explains when to leave it that way.
- Pronunciations play through MCI (MP3 and WAV); other formats need ffplay or mpv on PATH. HTTPS uses Windows'
  certificate store.
- The settings application downloads PaddleOCR with ONNX Runtime for Windows; Tesseract is used when it is installed
  (the UB Mannheim installer puts it in `Program Files\Tesseract-OCR`).
- Windows cannot delete or replace a file that is mapped. A dictionary removed or updated while the daemon has it open
  is renamed aside and deleted once nothing maps it any more.
- Pango's Windows font map takes a single family, not a list: the popup uses the first installed family of its list (Yu
  Gothic UI unless Noto Sans CJK JP is installed).
- Windows 11 rounds the corners of windows; the popup and highlight opt out and draw their own.

## macOS

`src/platform/macos` holds the backend: `MacBackend` (a listen-only event tap, the popup and highlight panels, the
pasteboard, the screen's scale and appearance), `AxCapture` (text through the accessibility API) and `CgScreen` (screen
pixels for OCR). The release is one universal app (Apple silicon and Intel) for macOS 13.3 or later, the first whose
libc++ formats floating-point numbers (`std::format`; `CMAKE_OSX_DEPLOYMENT_TARGET`). Its Pango, Cairo, HarfBuzz, GLib
and the rest are built for that by `.github/scripts/macos-deps.sh`, drawing with Core Text and Quartz (no FreeType,
Fontconfig or X11): Homebrew's and MacPorts' builds need the macOS they were built on.

### Building

With Xcode's command line tools (`xcode-select --install`), CMake 3.30+, Ninja, Meson and Qt's own universal build
(Qt's online installer, or [aqtinstall](https://github.com/miurahr/aqtinstall)):

```sh
pipx install cmake ninja meson
bash .github/scripts/macos-deps.sh ~/lexiglance-deps   # Pango, Cairo and theirs for macOS 13.3, a few minutes
pipx run --spec aqtinstall aqt install-qt mac desktop 6.8.3 clang_64 -O ~/Qt
cmake --preset release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos;$HOME/lexiglance-deps"
cmake --build --preset release
ctest --preset release
```

`cmake --install` makes `Lexiglance.app`: the settings application is its program, with `lexiglancectl` beside it in
`Contents/MacOS`, and the daemon is a helper app without a Dock icon, `Contents/Helpers/lexiglanced.app` (in
`Contents/MacOS`, LaunchServices would take it for Lexiglance.app itself). `.github/scripts/macos-package.sh` turns a
build into the disk image the release has:

```sh
bash .github/scripts/macos-package.sh bundle build/release build/arm64/Lexiglance.app   # Qt and libraries copied in
bash .github/scripts/macos-package.sh dmg build/dist build/arm64/Lexiglance.app         # signed ad hoc, in a .dmg
bash .github/scripts/macos-package.sh test build/dist/Lexiglance-macos.dmg              # as a user gets it
```

`dmg` merges the apps of several architectures into one universal app (CI builds each on a Mac of its own, and caches
the `macos-deps.sh` prefix until that file changes), and `test` checks that the programs start, load nothing from
outside the app and the system, and that every program and library runs on the app's `LSMinimumSystemVersion`. A
build against Homebrew's or MacPorts' Pango works on your own Mac but fails that last check.

### How it behaves

- Settings, dictionaries, models and the logs live in `~/Library/Application Support/Lexiglance`, caches in
  `~/Library/Caches/Lexiglance`.
- macOS asks for three permissions, which Lexiglance requests once and its health check reports: Input Monitoring for
  the trigger keys, Accessibility for the text under the pointer, Screen Recording for OCR (System Settings -> Privacy &
  Security).
- The release is signed ad hoc, as there is no Developer ID: the first start needs **Open Anyway** in System Settings ->
  Privacy & Security, and since every build's signature differs, macOS may ask for the permissions again after an
  update.
- The default trigger is Command + Left Option.
- Input Monitoring and Screen Recording take effect only in a new process: after allowing them, restart Lexiglance
  (the health check's **Restart** button).
- **Scanning -> Stop the window underneath from scrolling too** works as on Windows and needs Accessibility
  ([anticheat.md](anticheat.md#on-macos)).
- Pango lays the popup out with Core Text, so it finds the fonts macOS has without fontconfig.
- Pronunciations play through `afplay`.

Keep every backend passive (see [anticheat.md](anticheat.md)): observe input, never synthesise or grab it.
