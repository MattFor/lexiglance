# Debugging Lexiglance

Two tools help find out why Lexiglance does what it does: **Report a problem**, for everyone, and **developer mode**,
for people working on Lexiglance or chasing a bug with it. Neither sends anything anywhere, and neither sends input to
other programs: everything they simulate goes through Lexiglance's own pipeline, which stays a passive reader of the
screen ([anticheat.md](anticheat.md)).

## Reporting problems

**Overview -> Health -> Report a problem...**, **About -> Report a problem...** or the tray menu's **Report a
problem...** open a card over the window. Write what happened, tick whether a picture of the latest popup goes along
(off each time the card opens)
(it is drawn again from what it showed: the word and its entries, nothing else of the screen), and look at any part of
the report with **View** before anything is saved. **Save and open GitHub** then:

- saves `lexiglance-report-<date>-<time>.zip` among your downloads (else on the desktop) and shows the folder;
- opens GitHub's bug report form in the browser with what happened, the version, the system and a summary of the key
  facts filled in (the link stays under 7 KB);
- puts the same summary on the clipboard, in case the browser does not open.

Drag the zip into the form and send it. Nothing is uploaded by Lexiglance itself.

Without the settings application, `lexiglancectl report [file.zip] [--what "what happened"]` saves the same report in
the current folder and prints the link.

The report holds:

| File               | What                                                                                                                                                                           |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `report.txt`       | what happened, and the summary: versions, system (OS, desktop, X11 or Wayland), backend, text capture, scale, languages, dictionaries, the health report's errors and warnings |
| `status.json`      | the daemon's status (versions, backend, capture methods and what is missing, scale, theme)                                                                                     |
| `health.json`      | the health report, the last lookup on screen among it                                                                                                                          |
| `config.json`      | the settings, with the AnkiConnect key, passwords in addresses and secret URL parameters removed                                                                               |
| `dictionaries.txt` | the installed dictionaries' names, kinds and languages, not their contents                                                                                                     |
| `daemon.log`       | the last 1500 lines of the daemon's log (each lookup on screen is a line in it)                                                                                                |
| `application.log`  | the last 1500 lines of the settings application's log                                                                                                                          |
| `traces.jsonl`     | the Developer page's trace (the text read at each lookup, where, in which window, the timings), only when ticked                                                                                                                       |
| `popup.png`        | the latest popup, when ticked                                                                                                                                                  |

Your home folder is written as `~` throughout. `src/core/Report.cpp` builds all of it; `tests/TestReport.cpp` checks
that no secret gets through.

## Developer mode

Off by default and invisible. To turn it on, click the version on the **About** page seven times and tick **Developer
mode**, or start `lexiglance --developer`. It is saved in the settings (`"developer": true`) and turned off with the
page's **Turn developer mode off** or the same tick; turning it off forgets the trace and log the page gathered and the
problems it simulated. The **Developer** page then sits at the bottom of the sidebar.

The daemon answers the `debug.*` methods and lets clients subscribe to the trace and the log (`events.subscribe`) only
while developer mode is on: they hand out the screen (which macOS lets the daemon read), the log and what was looked up
to any program of yours that asks, so a program without macOS's Screen Recording permission cannot read the screen
through Lexiglance. Turned off, they answer with an error that says how to turn it on, and subscriptions stop.

### Trace

Every lookup, newest first, as it happens: its number (the daemon's generation of the request), time, kind (`scan`,
`sentence` with the sentence key, `wheel`, `text` for a selection or a simulated lookup), window class, capture source
(`at-spi`, `ui-automation`, `accessibility` on macOS, `ocr`), the text read and its offset in the sentence, the
capture's confidence, the entries found, and how long each stage took in milliseconds:

| Stage       | From                                                                                  |
| ----------- | ------------------------------------------------------------------------------------- |
| `capture`   | reading the screen (0 when the wheel reuses what was read)                            |
| `lookup`    | the dictionary lookup                                                                 |
| `translate` | the sentence's translation, when one was made                                         |
| `render`    | drawing the popup (twice: what fits on screen first, the rest once the pointer rests) |
| `show`      | from the drawn popup to it on screen, the wait for the desktop's thread included      |

Above the table, the median and 95th percentile of each stage over the latest 200 lookups. The daemon times and sends
any of this only while a client has subscribed to it (the page is open, or `lexiglancectl trace` runs); otherwise
the pipeline does no extra work for it.

### Capture

What the screen reads at a point (in screen pixels), or **at the pointer in 3 s** for pointing into another window: the
text, its offsets, the sentence, the box of the character there and of each of its lines. For OCR, the picture it read
the text from, with the lines it found drawn on it (green: read, grey: found but not needed, thin: the looser second
look), the captured lines (orange), the character (blue) and the pointer (red).

### Simulate

- **Scan there**: what holding the trigger at that point does (with the sentence key too, if ticked).
- **Look up there**: any text, through the pipeline as a selection at that point: looked up, translated as set, the
  real popup shown.
- **Wheel +1 / -1**, **Let go of the trigger**, **Hide the popup**: as the wheel, the trigger and a click do.
- **Health problems**: a notice, a warning (its fix opens Scanning), an error (its fix restarts the daemon), OCR
  missing, macOS's permissions refused. They show in the health report and the status as if real, until unticked or
  the daemon restarts.
- **Settings application**: the setup wizard again, the what's new card of any version, an update found or installed
  (on Overview -> Updates; nothing is downloaded).

### Log

The daemon's log as it is written (the last 1000 lines first), filtered by level, copied or saved. **The daemon logs**
sets its level from then on (saved in the settings; `lexiglanced --verbose` starts at debug).

### Console

Any IPC method ([ipc.md](ipc.md)) with JSON parameters, its answer, and the events that arrive.

### Export diagnostics

Saves the problem report above among your downloads and shows it, one click: with the page's trace (the text read at
each lookup, where and in which window), without the picture of the popup. In **Report a problem** the trace goes along
only when its tick is set, and that tick is offered only while the page has gathered one.

## From a terminal

```sh
lexiglancectl trace                              # the trace, one line per stage of each lookup
lexiglancectl debug capture 640 360 --image capture.png
lexiglancectl debug scan 640 360 [--sentence]
lexiglancectl debug lookup 食べた 640 360
lexiglancectl debug wheel +1                     # or -1
lexiglancectl debug release
lexiglancectl debug simulate error ocr           # no names: back to the real report
lexiglancectl debug log 200                      # the last 200 lines, then the log as it is written
lexiglancectl debug call config.get '{}'         # any method, its answer as it is
lexiglancectl report                             # the problem report, saved here
```

A daemon of a home of its own (`LEXIGLANCE_HOME=...`) is reached the same way with that variable set, which is how
these are tried without touching the Lexiglance you use; CONTRIBUTING.md's **Trying the keys and the wheel** runs one in
a display of its own.
