# Changelog

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
