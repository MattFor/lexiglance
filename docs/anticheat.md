# Anti-cheat safety

Lexiglance is designed not to look like, or behave like, cheating software. Anti-cheat systems flag programs that
touch a game's process, its memory or its input stream. Lexiglance does none of that.

## What doesn't do

- It never opens, reads, writes, traces (`ptrace`) or injects into another process, and never uses `LD_PRELOAD`.
- It never hooks graphics APIs, Vulkan layers or overlays inside other programs.
- It never synthesises input (no XTest, `uinput` or `SendInput`) and never grabs keys or the pointer. The one exception
  is the mouse wheel while the trigger is held over an open popup (see the table below), which can be turned off; on
  Windows that one has to be turned on first.
- It never reads `/dev/input`, and needs no root or extra permissions.
- It never records the screen. Only OCR looks at pixels: a region around the pointer (about 1000 × 500 pixels, cut to
  the window under it), only while the trigger is held, kept in memory and never saved.

## What it does

| Need                               | Mechanism                                                                               | Why it is safe                                                                                                                                                                |
|------------------------------------|-----------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Know when the trigger is held      | XInput2 raw events on the root window                                                   | Passive observation. Every key still reaches the focused application.                                                                                                         |
| Pointer position                   | `XQueryPointer`, only while the trigger is held                                         | A standard, read-only query.                                                                                                                                                  |
| Text under the pointer             | AT-SPI (desktop accessibility bus)                                                      | The application exports its text itself; games expose nothing.                                                                                                                |
| Text in games, images and video    | OCR of the root window (`XGetImage`)                                                    | The same request every screenshot tool makes, to the X server rather than the game.                                                                                           |
| Show the popup                     | An ordinary override-redirect window                                                    | It never takes keyboard focus, and the highlight overlay is click-through.                                                                                                    |
| Wheel changes the looked-up length | A grab of mouse buttons 4 and 5 only, only while the trigger is held over an open popup | The ordinary X11 button grab hotkey tools use, at the X server; released with the trigger. **Scanning -> Mouse wheel changes the looked-up length** off means no grab at all. |
| Selections (optional)              | The X11 PRIMARY selection                                                               | Standard clipboard protocol.                                                                                                                                                  |

## On Windows

The same rules, with Windows' own means. Nothing is grabbed unless you ask for it: by default the wheel is only
observed, so the window under the pointer still scrolls while it changes the looked-up length.

| Need                                             | Mechanism                                                                       | Why it is safe                                                                                                                           |
|--------------------------------------------------|---------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------|
| Know when the trigger is held                    | Raw Input with `RIDEV_INPUTSINK`                                                | Passive observation: no hook (`SetWindowsHookEx`) and no `RIDEV_NOLEGACY`, so every key and click still reaches the focused application. |
| Keyboard state after a lock screen or UAC prompt | `GetAsyncKeyState`                                                              | A read-only query.                                                                                                                       |
| Pointer position                                 | `GetCursorPos`                                                                  | A read-only query.                                                                                                                       |
| Text under the pointer                           | UI Automation (`TextPattern`)                                                   | The application exports its text itself; games expose nothing.                                                                           |
| Text in games, images and video                  | `BitBlt` of the screen                                                          | What every screenshot tool does, through the desktop compositor rather than the game.                                                    |
| Show the popup                                   | Layered windows with `WS_EX_NOACTIVATE`, the highlight also `WS_EX_TRANSPARENT` | They never take the focus, and the highlight lets every click through.                                                                   |
| Copied text (optional)                           | `AddClipboardFormatListener`                                                    | The clipboard's own change notification.                                                                                                 |

### Keeping the wheel from the window underneath

X11 can take just the two wheel buttons with an ordinary button grab. Windows has no equivalent, only
`SetWindowsHookEx(WH_MOUSE_LL, ...)` — a global low-level mouse hook, which is exactly the mechanism some anti-cheat
systems treat as suspicious in its own right, whatever it is used for.

So it is off by default and Lexiglance installs no hook at all. Turning on **Scanning -> Also keep the wheel from the
window underneath** installs one, and only while the trigger is held over an open popup; it comes down again the
moment the trigger is let go. It swallows the wheel and nothing else, and it never synthesises input. Even then the
trigger keys stay passive, because low-level hooks do not gate Raw Input — which is also how Lexiglance still sees the
turn it has taken away from the other window.

If you play anything with kernel-level anti-cheat, leave this off.

## Excluding games completely

**Scanning -> Ignored windows** takes window class patterns such as `steam_app_*`. While such a window is under the
pointer, Lexiglance does nothing at all. **Pause scanning** in the tray menu stops scanning everywhere, and
**Scanning -> Use OCR -> Off** disables screen reading entirely.
