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
backend only brings its screenshot call.

| Piece               | X11 (done)                     | Windows (done)                                     | macOS                                                                            | Wayland                                            |
|---------------------|--------------------------------|----------------------------------------------------|----------------------------------------------------------------------------------|----------------------------------------------------|
| Trigger keys        | XInput2 raw events             | Raw Input with `RIDEV_INPUTSINK` (no hooks)        | `CGEventTap` (listen-only)                                                       | GlobalShortcuts portal                             |
| Text under pointer  | AT-SPI                         | UI Automation `TextPattern.RangeFromPoint`         | `AXUIElementCopyElementAtPosition` + `kAXRangeForPositionParameterizedAttribute` | AT-SPI (the pointer position needs the compositor) |
| Screen pixels (OCR) | `XGetImage` of the root window | `BitBlt` of the virtual screen                     | `CGWindowListCreateImage`                                                        | Screenshot portal                                  |
| Popup window        | override-redirect              | layered `WS_EX_NOACTIVATE \| WS_EX_TOPMOST` window | borderless non-activating `NSPanel`                                              | layer-shell surface                                |
| Highlight           | shaped, click-through window   | layered window with `WS_EX_TRANSPARENT`            | ignoring-mouse `NSWindow`                                                        | layer-shell surface                                |
| Selection lookup    | PRIMARY selection              | the clipboard                                      | the pasteboard                                                                   | primary selection protocol                         |
| Audio player        | ffplay, mpv, mpg123            | MCI (DirectShow), else ffplay or mpv               | `NSSound` / AVFoundation                                                         | as X11                                             |
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
- The default trigger, Win + Left Alt, opens neither the Start menu nor the menu of the application below.
- UI Automation reads the applications that implement its text pattern: Office and WordPad, browsers, Qt, WPF and WinUI
  applications, Windows Terminal. The classic Win32 edit control (Notepad on Windows 10) has none; OCR reads it.
- Selection lookup becomes lookup of copied text: Windows has no selection of its own, and text hookers copy what they
  read to the clipboard.
- The wheel changes the looked-up length while the trigger is held, and by default the window under the pointer scrolls
  too, since taking the wheel from it needs a low-level mouse hook. **Scanning -> Also keep the wheel from the window
  underneath** installs one while the trigger is held over a popup; it is off by default, and
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

Keep every backend passive (see [anticheat.md](anticheat.md)): observe input, never synthesise or grab it.
