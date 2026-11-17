# Changelog

## [1.5.0] - 14:50 CET 17-11-2026

### Added

- macOS 13.3 Ventura or later, on Apple silicon and Intel: `Lexiglance-macos.dmg` holds Lexiglance.app, with the daemon,
  `lexiglancectl` and everything they need inside; drag it onto Applications. The trigger is Command + Left Option.
  Lexiglance asks for Input Monitoring, Accessibility and Screen Recording (for OCR), and the health check reports
  which are missing. It is not notarised: the first time, open it, then choose Open Anyway in System Settings ->
  Privacy & Security; macOS may ask for the permissions again after an update.
- **Report a problem** (Overview -> Health, About, the tray menu): describe what happened, look at what the report
  holds, and Lexiglance saves it as a zip among your downloads and opens GitHub's issue form filled in with the key
  facts, to drag the zip into. It holds the versions, the system, how the screen is read, the health report, the
  settings without the AnkiConnect key or other secrets, the dictionaries' names, the end of the logs and, if ticked, a
  picture of the latest popup. Nothing is sent by Lexiglance itself. `lexiglancectl report` makes the same report.
- Developer mode, for working on Lexiglance (About: click the version seven times, or `lexiglance --developer`): a
  Developer page with every lookup traced live (what was read where, through what, and how long reading, looking up,
  translating, drawing and showing took, with medians and 95th percentiles), a capture inspector that shows the boxes
  found and the picture OCR read, scans, lookups, the wheel and health problems simulated through Lexiglance's own
  pipeline, the daemon's log as it is written, an IPC console and a diagnostics export. `lexiglancectl trace` and
  `lexiglancectl debug ...` do the same from a terminal; the daemon refuses all of these while developer mode is off.
  [docs/debugging.md](docs/debugging.md) has the details.
- Recent lookups: the tray menu lists the last ten words looked up on screen, and the Search page the last 30 while
  nothing is typed in it; a click looks one up again. They are kept with the statistics, on this computer only: not
  while counting is off, and forgotten with them. Scripts get them with the daemon's `stats.recent`.
- The tray menu (the menu bar icon on macOS) can turn scanning off in the program you looked something up in last,
  and on again: **Don't scan in** adds its window class to Scanning -> Ignored windows, which offers the same with a
  button, so the class no longer has to be found out by hand. The menu also opens Search, the icon fades while scanning
  is paused, and its tooltip names the trigger.
- Search: **Play**, **Copy** and **Add to Anki** under every entry. Up, Down, Page Up and Page Down scroll the results
  while typing, Escape empties the box, and Watch clipboard stays ticked from one start to the next.
- **Overview -> Settings -> Export** saves every setting in one file, and **Import** on another computer takes them
  over after listing what changes. The dictionaries are not in it.
- Changes to `config.json` made by hand (or synced from another computer) take effect within two seconds, with no
  restart. Lexiglance does not write such a file back, and dictionaries installed only on the other computer keep their
  place in it, so two computers sharing it settle on one file. A file that cannot be read leaves the settings in use as
  they are: the health check's recent problems say why, and the edit is kept as `config.json.broken`.

### Changed

- Words that match the text equally well are listed by how common they are, as your frequency dictionaries rank them:
  行って and 行っちゃった show 行く first, no longer 行う, which JMdict's own order puts first. The frequency dictionary
  that ranks most of them decides; without one, the order stays as it was.
- The daemon answers while a request that waits is under way: a health check, a translation, a note added to Anki or a
  pronunciation played for one program (the settings application, `lexiglancectl`, a script) no longer holds up the
  others, which waited for it for up to a minute. Each program still gets its answers in the order it asked.
- Appearance -> Scale goes from Automatic straight to 0.5x, the smallest scale a popup can still be read at; a smaller
  one typed in, or written in the settings file by hand, becomes 0.5x.
- Dictionaries dropped anywhere on the settings window are imported, not only on the Dictionaries page, which comes
  up to show their progress; a dropped folder of zips imports each of them.
- OCR: moving along a line of game or video text no longer looks for text again every few words: a line read once
  serves wherever the pointer goes along it. On the test page the slowest tenth of popups came after 150 ms and now
  after 20 ms, and the OCR benchmark reads 80.7% of its lines exactly instead of 78.3% (errors 4.5% instead of 5.5%),
  as lines are less often cut at the edge of what was read.
- OCR: a line read surely in the script of a language's own recogniser is not read again by the others: with Russian
  on as well, Japanese lines are read about a fifth sooner.
- Lookups search only the dictionaries of the text's script: Russian text no longer brings up a Japanese dictionary's
  stray entry first (Pixiv's Москва, read もすくわ), nor Greek text Japanese entries for Greek letters, and a lookup
  reads 15 to 80% less of the dictionary files, which makes the first lookups after a long break quicker.
- The first popup after Lexiglance starts is drawn in about half the time, as its fonts are loaded beforehand.
- First-run setup: OCR for games and videos is offered as a choice beside translation. **Continue while it downloads**
  is there from the start, so a slow or missing connection no longer keeps you on the download page for minutes, and
  when downloads fail the page names every one of them, grouped by what went wrong. The sentence to try comes with a
  couple of everyday words, and the page says when the dictionary is still on its way. Closed during the downloads, setup
  opens again where you left it, with your languages still ticked.
- Help has **Run setup again** and **What's new** for everyone, not only in development builds.
- The what's new card reads like the changelog it comes from: headings in proportion, space between the points, and
  links that open.
- The cards over the window (setup, Help, What's new, Report a problem, Uninstall) press their main button with Enter,
  and the keyboard goes back where it was when they close.
- The text throughout the settings window, the health check and the popup's messages is shorter and plainer, with one
  name for each thing and what to do next. On Windows the uninstall card also lists the autostart, the Start menu
  entry, the Installed apps entry and the Run as administrator task it removes.

### Fixed

- First-run setup: the sentence to try is read through accessibility (AT-SPI, UI Automation, macOS Accessibility).
  Pointing at a card over the settings window reached the page under it, so the sentence was not found there, nor by
  screen readers.
- Windows: with Run as administrator on but not in effect, Health no longer tells you to turn it on, which
  contradicted the start-up problem above it.
- Japanese written with emphasis is looked up as the word it is: うまーい, でかーい and やばーい as うまい, でかい and
  やばい, ほんっと as ほんと, すごぉい, かわいぃ and ありがとぅ as すごい, かわいい and ありがとう. The emphasis does
  not count towards how much of the text such a word matches, so a word written as the text is and as long still comes
  first: おーきい shows おー, not 沖.
- Firefox and other browsers: the last words of a paragraph (those less than the longest lookup from its end, often
  all of a short one) are read through accessibility; nothing at all was read there.
- OCR reads the whole of a line whose words the detector boxed apart when one of them has only short letters
  (изменения beside перед выходом?): the line used to stop at that word.
- Importing a dictionary no longer leaves a `.lgd.lock` file beside it in the dictionaries folder, and those earlier
  versions left are removed, as is the half-written file of an import that was killed.
- Linux and macOS: a daemon whose runtime folder is too deep for a socket (a long `LEXIGLANCE_HOME`) removes the
  `lexiglance-<user>-<id>` folder it made for the socket in the temporary folder when it exits.
- `lexiglancectl show` without a text shows a word the dictionaries have, as the daemon allows. Wrong arguments
  (`stats foo`, `status extra`, `import` with nothing to import, `translate --language` without a code) print the
  usage and exit with 2, instead of being ignored or ending without a word; `lexiglancectl --help` exits with 0.
- Uninstalling a copy with a home of its own (`LEXIGLANCE_HOME`) stops only that home's daemon and settings window,
  no longer every Lexiglance program of the user.
- When the settings application cannot reach its daemon, Overview names its own home's daemon, not another copy's.
- macOS: Chrome, Edge and other Chromium browsers have their pages' text read through accessibility, with the
  highlight split line by line; every lookup there used to fall back to OCR. Chromium and Electron apps are asked for
  their text as soon as they come to the front, so the first lookup in an app just started reads it too.
- macOS: after allowing Input Monitoring or Screen Recording, macOS's own Quit & Reopen restarts the daemon too, which
  until then kept running without the permission, so the trigger still did nothing.
- macOS: `lexiglance --claim-entries` says what it set up there, the autostart of the app in the menu bar, rather than
  a menu entry that a Mac does not have.
- Dictionaries: the next queued download no longer starts while one is still being imported, when some other import (a
  local file, the setup wizard) finishes meanwhile.
- A translation model that fails to load is written to the log and to the health report's recent problems, not only
  shown in the popup.
- `lexiglanced --replace` and the health check's Restart stop only the daemon of their own home (`LEXIGLANCE_HOME`).
- Windows: a copy with a home of its own (`LEXIGLANCE_HOME`) runs a daemon of its own, instead of reaching the installed
  copy's.
- Windows: the Translation page shows the compact models' size in full ("115 MB", not "115 ...").
- Windows: the Anki page says pronunciations play through Windows' own player, not ffplay, mpv or mpg123.
- Linux: text in Qt applications' text views (QTextEdit, QTextBrowser) is read through accessibility. It was not found
  there, so only OCR could read it.
- Holding the trigger again over text chosen with the wheel looks it up again. Until it was held somewhere else, no
  popup came there, and the wheel and the sentence key did nothing.
- Quick presses of the sentence key each count when popups are slow to draw too: a translation asked for by an earlier
  press, arriving after a later one, no longer comes back on screen to make the next press turn translation off rather
  than on.
- A pronunciation plays at once while Anki does not answer (a dialog open in it, a sync): it waited up to 20 seconds
  behind the notes being added and the popup's check for notes Anki already has.
- Of pronunciations asked for in a row (playing by themselves as the pointer moves on, the speaker clicked on another
  entry), the last one is heard: an earlier one still downloading no longer plays over it.
- Windows: words in Notepad, dialog boxes and other classic text fields are read directly over the right half of a
  character too. There they went through OCR, slower, and not at all with OCR off or not downloaded.
- Windows: turning Run as administrator on no longer reports "Restart failed" when Windows takes more than 15 seconds
  to start Lexiglance as administrator.
- First-run setup: when nothing of the dictionaries or models could be downloaded (offline, say), their progress bars
  no longer keep moving as if the downloads went on.

## [1.4.1] - 15:47 CET 16-11-2026

### Changed

- After an update the window shows what changed in every version since the one it last showed, the newest at the top,
  on a card that scrolls when that is long. This covers updates that came while the window was closed or in the
  background, and those from a package or an AppImage put in place, where only the last of several updates used to
  be shown, or none.

### Fixed

- Linux: a menu entry changed by hand keeps the changes. Pointing the entry at the copy just installed used to write
  Lexiglance's own entry over it; now only the program it starts changes. An entry Lexiglance wrote and nobody changed
  is still brought up to date.
- Linux: Lexiglance finds its menu entry whatever the file is called and in whichever applications folder it is: one
  made by AppImageLauncher or Gear Lever, one of your own, one that starts it through a wrapper (`env`, `ime-launch`).
  It no longer adds a second entry beside one, points one at the copy installed once the program it starts is gone,
  and the menu option on the Overview page hides it (yours in place) and brings it back.

## [1.3.9] - 14:42 CET 16-11-2026

### Fixed

- Linux: the menu entry is filed under Utilities (Accessories in XFCE) as a dictionary, where it used to open a menu of
  its own under Education. Moving it there by hand did not last: the entry is written again whenever its program path
  stops resolving or the menu option is changed.

## [1.3.8] - 11:38 CEST 10-11-2026

### Added

- Windows: the installer can leave the settings window out (a box on its welcome page, `/NOGUI` for a silent
  install), for the daemon and `lexiglancectl` alone. An update keeps it out.

### Changed

- The sentence key acts on every press, however quickly it is pressed: where a translation was asked for it turns it
  off, anywhere else it translates. A press no longer waits for the one before it, and pressing again translates
  nothing twice.
- A translation nobody is waiting for any more (the wheel turned on, the pointer moved to another sentence) is given
  up, so the one that is wanted comes sooner.

### Fixed

- OCR: with the recognisers of several languages loaded, a line could come back as « .» or spelled in look-alike
  letters of another script (Сохранить as CoxpaHuTb), because the recogniser that did not know its script was the
  surest of what it read.
- OCR: large text, and long lines pointed at near their end, are read whole. A line that runs on past the area that
  was read is read again from further out; sentences in letters of 48 px and more used to lose their beginning.
- Text chosen with the wheel no longer snaps back to the word when the hand on the wheel moves the pointer by a few
  pixels.
- Turns of the wheel where nothing was found are no longer applied to the next popup.

## [1.3.7] - 12:14 CEST 01-11-2026

### Added

- The AppImage carries update information, so AppImageUpdate and similar tools can update it; each release has its
  `.zsync` beside it.

### Changed

- The AppImage is now `Lexiglance-x86_64.AppImage`, spelled as the application is; copies of 1.3.6 and older still
  update to it, and old links still download it.
- Dictionaries import with at most three parser threads: no slower, and with less than half the memory on machines
  with many cores.
- A dictionary file over 128 MB is refused, and the banks being parsed are kept under 128 MB together, so a hostile
  archive fails its import instead of using up the memory.

### Fixed

- A settings file that cannot be read is kept as `config.json.broken` instead of being overwritten with the defaults.
- Settings changed before the settings window has reached the daemon, or while it is away, are no longer lost or
  pushed over the saved ones; **Reset all** waits for them to load.
- Two saves at the same moment no longer corrupt `config.json` or `statistics.json` or drop a setting, and two imports
  of the same dictionary no longer corrupt it.
- Dictionaries and settings that start with a UTF-8 byte order mark load, and so do archives with empty compressed
  files.
- Russian pronouns no longer cost up to 100 ms per lookup, long entries render several times faster, and a popup that
  already shows everything is not rendered a second time.
- A crafted dictionary can no longer hang the popup (furigana) or crash the daemon.
- Structured-content styles such as `transparent`, `inherit` or `none` no longer turn a definition into raw tags.
- Translation no longer drops words from a sentence that repeats a phrase.
- Anki: cards get the sentence the word is in, not the first one on the line; cloze templates such as
  `{{c1::{expression}}}` are filled; text from the screen or a dictionary is escaped, so `<int>` shows and no markup
  runs; check marks survive moving the pointer; a deck list that fails to load (Anki at its profile chooser) no longer
  empties the chosen deck; `anki.add` fetches no audio when audio is off.
- A client that stops reading no longer stalls the daemon, and `popup.show` while the desktop is busy no longer
  writes into freed memory.
- The setup wizard waits for each dictionary to be imported before it restarts the daemon.
- Downloads that stall end after 30 s instead of keeping updates and setup busy, and a dictionary's update check reads
  at most 4 MB.
- Installing OCR and a translation model at once no longer unpacks ONNX Runtime twice.
- **Fix issues** no longer pauses scanning again, and the Dictionaries page changes the dictionary you clicked even
  right after an import.
- Holding the trigger while settings change no longer stops lookups or leaves the popup up after the release.
- The diagnosis zip leaves out the AnkiConnect key, and `lexiglancectl` replaces control characters in what it prints.
- Linux: a terminal of another AppImage (an editor's, for example) no longer makes Lexiglance take that AppImage for
  its own when it updates, uninstalls or starts the daemon.
- Linux: the AppImage no longer looks for libraries in the folder it is started from.
- Linux: without `XDG_RUNTIME_DIR`, the settings window and `lexiglancectl` refuse a daemon socket another user made,
  and the settings window's own socket is no longer in the shared `/tmp`.
- Linux: AT-SPI reads Flatpak applications, and no longer reads a window hidden behind one without a process id.
- Linux: Wine games in override-redirect windows match the default OCR windows, and OCR reads 30-bit and 16-bit
  displays correctly.
- Linux: the updater and other programs the daemon starts no longer inherit its blocked signals.
- Linux: pronunciation audio over HTTPS works with the AppImage where the certificates are kept elsewhere (openSUSE),
  and older libcurl is held to HTTP and HTTPS as well.
- Linux: Health no longer says no audio player is installed when `gst-play-1.0` or `pw-play` plays the audio.
- PaddleOCR reads lines again once the popup no longer covers them, and no longer returns text from an earlier frame.
- Statistics no longer re-sort every word on each new word once 2000 are counted, and count new words again.
- Windows: UI Automation calls time out on a hung application instead of stalling lookups.
- Windows: touchpad scrolling in the popup moves by the amount scrolled, and a word wrapped across lines gets a
  highlight on each line.
- Windows: statistics are saved at logoff and shutdown, and recording a key chord no longer ends at the first key let
  go.
- `.github/scripts/dev-install.sh` installs service files that start the daemon from the prefix it installs to.

## [1.3.6] - 08:24 CEST 30-10-2026

### Fixed

- A disk that fills up while the settings or statistics are saved no longer leaves them empty: the saved ones stay.
- Without `XDG_RUNTIME_DIR`, the daemon no longer uses a `/tmp` folder (or a link) that belongs to another user, which
  could have had it write its lock file over one of yours.
- Updating or uninstalling no longer stays busy for good when `pkexec` cannot start.
- Pronunciation audio and Anki answers stop at 64 MB however much a compressed answer would grow (the AppImage's older
  libcurl only checked the size a server announced).

## [1.3.5] - 14:00 CEST 15-10-2026

General improvements to auto-replacement on update + better branding.

- Daemon now gets properly replaced when an update happens.
- Look interval reduced from 10 min -> 1 min for faster update/replacement detection.
- GUI automatically restarts on update.
- Updated the icons

## [1.3.4] - 21:00 CEST 05-10-2026

Adjustments so you can run as administrator in order to get words from higher elevation programs.

- Windows: **Overview -> Run as administrator**, for games that run as administrator (Windows keeps their keys from
  Lexiglance otherwise). Windows asks once; Health points to it when such a program is in front.
- Windows: the installer starts with a welcome page, and Lexiglance comes to the front afterwards instead of flashing in
  the taskbar.
- The popup goes when you let go of the trigger, unless the pointer is on it (move onto it first to click an entry).
- Windows: the daemon no longer stops answering when something checked on it at the wrong moment.
- The autostart and menu entry follow the copy installed last; after an update only what changed is shown.

## [1.3.3] - 14:00 CEST 21-09-2026

Miscellanous fixes.

## [1.3.2_1] - 13:10 CEST 21-09-2026

Quick hotfix to resolve icon issues.

## [1.3.2] - 10:50 CEST 21-09-2026

Quick hotfix to resolve focus issues.

- The settings window no longer jumps to the front every six hours when the update check runs.

## [1.3.1] - 10:15 CEST 21-09-2026

- Manual colour picking no longer obscures text (Windows)
- The trigger mends itself within a second when a key release or the raw-input registration goes missing,
  instead of staying stuck until a restart. (Windows)
- OCR: a line the detector cuts in two on the same row (at a bracket, at a quotation mark) reads as one run of text
  again, not just the piece before the cut.

## [1.3.0] - 01:00 CEST 21-09-2026

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

## [1.2.0] - 18:00 CEST 18-09-2026

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

## [1.1.0] - 22:20 CEST 16-09-2026

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

## [1.0.1] - 23:15 CEST 14-09-2026

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

## [1.0.0] - 20:00 CEST 14-09-2026

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

[1.5.0]: https://github.com/MattFor/lexiglance/releases/tag/v1.5.0
[1.4.1]: https://github.com/MattFor/lexiglance/releases/tag/v1.4.1
[1.3.9]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.9
[1.3.8]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.8
[1.3.7]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.7
[1.3.6]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.6
[1.3.5]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.5
[1.3.4]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.4
[1.3.3]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.3
[1.3.2_1]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.2_1
[1.3.2]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.2
[1.3.1]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.1
[1.3.0]: https://github.com/MattFor/lexiglance/releases/tag/v1.3.0
[1.2.0]: https://github.com/MattFor/lexiglance/releases/tag/v1.2.0
[1.1.0]: https://github.com/MattFor/lexiglance/releases/tag/v1.1.0
[1.0.1]: https://github.com/MattFor/lexiglance/releases/tag/v1.0.1
[1.0.0]: https://github.com/MattFor/lexiglance/releases/tag/v1.0.0
