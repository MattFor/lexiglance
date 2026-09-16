# Changelog

## [1.1.0] - 22:20 CEST 2026-09-16

- Statistics page: lookups, languages, sources and recent days, kept locally (optional).
- Middle-click in the popup plays the pronunciation under the pointer. Speaker and Anki buttons are larger by default;
  Appearance → Shown → Button size sets how large.
- The popup closes when another window takes the keyboard (for example Alt+Tab).
- Highlight padding can be set sideways and vertically separately, including negative values.
- Daemon and application logs (`daemon.log`, `application.log`) with wall-clock times, thread names and richer detail;
  linked from About → Files.
- Help: click a number field, then use the wheel to change it quickly; 0 means automatic where the field says so.
- Windows: OCR installs the Visual C++ Redistributable when needed (health check, Download PaddleOCR, the setup, and
  silent self-updates). Lexiglance restarts on the new runtime by itself.
- Overview → Health → Fix issues carries out every automatic fix at once.
- About shows build channel (stable/dev) and platform beside the version.
- Overview tagline sits beside the Lexiglance title; Text capture tile wraps the bracketed detail onto its own line.
- Windows: each pronunciation clip uses its own temp file, so replay no longer fails after the first play.
- Windows: the Windows key is shown as Win rather than Super in settings, health, help and the log.
- Windows: optional Scanning setting to keep the wheel from the window underneath while it changes looked-up length
  (off by default; see docs/anticheat.md).
- Health and About turn download addresses into links; capture failures are logged as warnings under Recent problems.

## [1.0.1] - 23:15 CEST 2026-09-14

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
