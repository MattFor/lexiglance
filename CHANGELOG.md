# Changelog

## [1.0.1] - 2026-09-14

- Windows: PaddleOCR no longer fails after **Download PaddleOCR** with "Unsupported model IR version: 10, max
  supported IR version: 9". Lexiglance used the older ONNX Runtime that Windows keeps for itself; it now uses only the
  one it downloads. On Linux too, OCR downloaded while Lexiglance runs works without restarting it.
- Help: on the first start, a short card lists what you can do and where. **Help**, at the bottom left, shows it
  again.
- **Overview -> Startup -> Also start this window, hidden in the tray**: the settings application starts at login as a
  tray icon.
- Every page updates as things change: OCR's state right after a download, the health checks, About, and Anki when it
  starts or closes. No need to switch pages.
- The mouse wheel scrolls every page again; before, it did nothing until a page's scroll bar had been clicked.
- Downloads show one steady progress bar with the sizes on it, instead of text that jumped between files.
- Updates: **Overview -> Updates** installs the newest release (**Update to …**, or **Reinstall …** when this is
  already it) and, unless turned off, does so on its own. Each copy gets the build it was installed from: the Windows
  setup, the portable .zip, the AppImage or the .deb (which asks for your password), checked against the release's
  SHA256SUMS, then Lexiglance opens again.

## [1.0.0] - 20:00 CEST 2026-09-14

First public release.

- System-wide pop-up dictionary for Linux (X11) and Windows 10 and 11: hold the trigger keys and point at a word in any
  application.
- Japanese, Russian, Ukrainian, Korean and Greek. Each language is a JSON file, and you can
  add your own without rebuilding.
- Yomitan dictionaries, compiled into memory-mapped indexes for lookups in microseconds. Recommended dictionaries for
  each language install in one click.
- Finds inflected words: about 400 Japanese and 500 Russian rules, plus the inflected forms Wiktionary dictionaries
  list.
- Reads text through accessibility (AT-SPI, UI Automation), or with OCR (PaddleOCR or Tesseract) in games, images and
  videos.
- Pronunciations from JapanesePod101, Wikimedia Commons or a custom server, and Anki cards through AnkiConnect.
- Popup designs, colour schemes, themes and highlight styles.
- A health check that finds problems and offers fixes.
- `lexiglancectl` for scripts, and service files for systemd, runit, OpenRC, dinit, s6 and XDG autostart.
