# Roadmap

## Known limitations

- In VTE terminals the highlight can sit slightly off on wide characters. The text that is looked up is correct.
- OCR can confuse similar kanji in very small pixel fonts, and it skips lettering drawn as artwork.
- On Windows the window under the pointer scrolls while the wheel changes the looked-up length, and Notepad on
  Windows 10 can only be read with OCR.
- Wayland works only through XWayland, and macOS has no desktop support yet.

## Planned

- Looking up words inside the popup.
- Kanji cards for Anki.
- Images from dictionary entries in the popup.
- Pitch accent graphs.
- Wayland and macOS support ([porting.md](porting.md)).
- More languages. Chinese needs its own handling, and Latin-script languages need a way to be told apart from English
  interface text.
- A setting for the preferred language when several languages share a script.
