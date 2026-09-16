# Daemon protocol

`lexiglanced` listens on `$XDG_RUNTIME_DIR/lexiglance/daemon.sock` (a Unix domain socket, owner only). Messages are
single-line JSON objects.

One daemon runs per user: it holds an exclusive lock on `$XDG_RUNTIME_DIR/lexiglance/daemon.lock`, which records its
pid. A second `lexiglanced` exits; `lexiglanced --replace` takes over instead: it asks the running daemon to shut down,
ends it (SIGTERM, then SIGKILL) if it has not exited within five seconds, and ends stray daemons without the lock too.
A daemon that is shutting down closes its socket first and ends itself if its threads have not stopped within four
seconds.

```
-> {"id": 1, "method": "lookup", "params": {"text": "食べた"}}
← {"id": 1, "result": {...}}          or   {"id": 1, "error": {"message": "..."}}
← {"event": "import.progress", "params": {...}}     (broadcast to every client)
```

## Methods

| Method                        | Parameters                                                   | Result                                                                                                                                                                              |
|-------------------------------|--------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `status`                      |                                                              | version, pid, uptime, backend, capture (a summary) and capture_problem (why a part of it is missing, in full), paused, scale, dark_theme, dictionaries, languages in use, lookup statistics, paths                                                       |
| `config.get`                  |                                                              | `{config}`                                                                                                                                                                          |
| `config.set`                  | `{config}`                                                   | saved and applied immediately                                                                                                                                                       |
| `scan.pause`                  | `{paused?}`                                                  | toggles when `paused` is omitted                                                                                                                                                    |
| `health`                      | `{interactive?}`                                             | `{checks: [{id, title, status: "ok" \| "info" \| "warning" \| "error", detail, fix}], errors, warnings}`; `interactive` means a click started it, so input events must have arrived |
| `dictionaries.list`           |                                                              | installed dictionaries in priority order, each with its `kind` ("Words", "Names", "Monolingual", "Kanji", ...)                                                                      |
| `dictionaries.sort`           | `{dry_run?}`                                                 | `{order: [{title, kind}]}`: the smart order, applied unless `dry_run`                                                                                                               |
| `dictionaries.import`         | `{path, replace?, delete_source?, replaces?}`                | `{job}`; progress arrives as events                                                                                                                                                 |
| `dictionaries.remove`         | `{title}`                                                    |                                                                                                                                                                                     |
| `dictionaries.reload`         |                                                              |                                                                                                                                                                                     |
| `lookup`                      | `{text, markup?: "html" \| "plain" \| "pango", max_length?}` | terms, kanji, timing                                                                                                                                                                |
| `popup.show` / `popup.hide`   | `{text}` /                                                   | shows a popup at the pointer                                                                                                                                                        |
| `popup.preview`               | `{text, config?}`                                            | `{png (base64), width, height, scale}`                                                                                                                                              |
| `highlight.preview`           | `{config?}`                                                  | `{png (base64), width, height, scale}`: the highlight over sample text, light and dark                                                                                              |
| `keys.record` / `keys.cancel` |                                                              | the chord arrives as the `keys.recorded` event                                                                                                                                      |
| `debug.capture`               | `{x, y}`                                                     | the text the capture backend reads at that point                                                                                                                                    |
| `debug.scan`                  | `{x, y}`                                                     | a whole lookup at that point, as holding the trigger there would do (capture, popup, highlight)                                                                                     |
| `audio.play`                  | `{expression, reading}`                                      | plays the term's pronunciation                                                                                                                                                      |
| `anki.add`                    | `{text, entry?, sentence?}`                                  | `{note}`: looks `text` up and adds that entry to Anki                                                                                                                               |
| `capture.reset`               |                                                              | rebuilds the text capture (after installing an OCR model)                                                                                                                           |
| `stats`                       |                                                              | the tally kept between runs: `{enabled, first_day, days_used, sessions, lookups, found, characters, popups, audio, anki, distinct_words, average_lookup_us, words: [{word, count}], languages: [{language, count}], sources: [{source, count}], days: [{day, count}]}` |
| `stats.reset`                 |                                                              | forgets everything counted so far                                                                                                                                                   |
| `shutdown`                    |                                                              |                                                                                                                                                                                     |

## Events

`config.changed`, `status.changed`, `dictionaries.changed`, `import.started`, `import.progress`,
`import.finished`, `keys.recorded`, `trigger.changed` (`{held}`: the trigger chord was pressed or let go) and
`capture.result` (`{summary, where, found, entries}`: what the latest lookup on screen read, a few times a second at
most).
