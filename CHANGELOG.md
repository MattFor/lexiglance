# Changelog

## [1.3.2] - 10:50 CEST 2026-09-21

Quick hotfix to resolve focus issues.

- The settings window no longer jumps to the front every six hours when the update check runs.

## [1.3.1] - 10:15 CEST 2026-09-21

- Manual colour picking no longer obscures text (Windows)
- The trigger mends itself within a second when a key release or the raw-input registration goes missing,
  instead of staying stuck until a restart. (Windows)
- OCR: a line the detector cuts in two on the same row (at a bracket, at a quotation mark) reads as one run of text
  again, not just the piece before the cut.

## [1.3.0] - 01:00 CEST 2026-09-21

- You are now able to translate sentences using scroll-wheel selection.
- Russian, Ukrainian, Korean and Greek: pointing anywhere in a word looks up the whole word (книгу, not игу).
- OCR keeps the spaces between words, ends the highlight on the word's own ink, mends letters read in the wrong script
  and reads game fonts far better (Russian errors down from about 210 to 79 in 1800 letters); a caption, a smaller line
  or a second column no longer reads as the sentence going on.
- Statistics page now looks way better.
- Updates install in the background even with the settings window closed; Fedora gets an `.rpm` package.
- Uninstalling from **Overview**, **Settings -> Apps** on Windows or `lexiglancectl uninstall`.
- The first-run setup, redone, ending on a sentence to try the popup on; every language now has such an everyday
  `example_sentence`, which Health translates as its test.
- Fixes: the Linux programs go by their own names again (a stray daemon was not ended, Health and the install scripts
  missed them); a dismissed popup could come back; clicking an entry under a wheel selection acted on the one below it;
  the AppImage's autostart ran the daemon from its temporary mount.

## [1.2.0] - 18:00 CEST 2026-09-18

Updates and bugfixes reported from: https://github.com/MattFor/lexiglance/issues/7

- Health also reports languages, recommended dictionaries still missing
  for the preferred language, and whether the data, log and download folders can be written.
- No more copy system information within about, now it generates a full zip file raedy to share with all relevant
  information.
- First-run setup: Welcome → choose languages → dictionaries and OCR install themselves.
- Modifier and mouse keys are shown with everyday names program-wide (Left Alt, Left Ctrl, Left Win / Left Super,
  Caps Lock, Left click, and so on). Config files still accept the older spellings.
- Scanning: Preferred language can be chosen among those turned on (needed when scripts overlap, e.g. Russian and
  Ukrainian). Turning a language off moves Preferred to another that is still on. A note says at least one language
  must stay on.
- Health warns when OCR models are installed for some enabled languages but not others.
- Health: "Show the N passed checks too" stays above the list instead of jumping under new rows.
- Scrolling the page under the pointer (without expanding the looked-up length) closes the popup.
- When accessibility returns only English chrome beside an image, OCR still runs so README figures and similar work.
- Windows: after dismissing the popup with a click, holding the trigger (especially Win+Alt) could leave lookups
  working in the log while the popup never appeared again until restart. Focus changes while the trigger is held are
  ignored, and layered popups are shown again with `ShowWindow` after hide.
- The settings window is brought to the front when first-run or post-update setup opens.

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
- Windows: optional Scanning setting to keep the wheel from the window underneath while it changes looked-up length (off
  by default; see docs/anticheat.md).
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
