# Themes

A theme is the look of the popup and of the highlight: the design, the colours, the window and the sizes. What the
popup shows (frequencies, pitch accent, ...), how wide it is and where it opens stay your own settings.

## Using themes

**Appearance -> Themes** lists the built-in themes and your own. Choosing one applies it at once; every part of it can
still be changed afterwards. **Save as theme...** keeps the current look as a theme of your own.

Built in: **Friendly** (the default: large furigana above the kanji, plain words instead of dictionary codes, numbered
senses), **Classic (Yomitan)**, **Compact**, **Minimal**, **Paper**, **Nord**, **Sakura**, **Matcha**, **Midnight** and
**High contrast**. **Appearance -> Highlight -> Ready-made highlights** sets only the highlight.

## Theme files

Your themes are JSON files in `~/.config/lexiglance/themes/`. To share one, pass the file on; **Themes -> Import a theme
file...** copies one in. They are easy to write by hand:

```json
{
  "name": "Evening paper",
  "lexiglance_theme": 1,
  "popup": {
    "design": "friendly",
    "scheme": "paper",
    "accent_color": "#e39a64",
    "colors": {
      "muted": "#a89a86",
      "tag_name": "#d46a8c"
    },
    "corner_radius": 12,
    "border_width": 0,
    "padding": 14,
    "opacity": 95,
    "furigana_size": 18,
    "highlight_style": "fill",
    "highlight_color": "#e39a6455",
    "highlight_radius": 4,
    "highlight_padding": 2
  }
}
```

Settings a theme leaves out take their defaults, so a theme looks the same whatever was set before it. The names are
those of the `popup` section of `config.json`.

| Setting                                                          | Values                                                                                                                                                 |
|------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|
| `design`                                                         | `friendly`, `classic`, `compact`                                                                                                                       |
| `scheme`                                                         | `default`, `paper`, `nord`, `sakura`, `matcha`, `midnight`, `contrast`; each has a dark and a light variant, chosen by **Appearance -> Dark or light** |
| `background_color`, `text_color`, `accent_color`, `border_color` | `#rrggbb`; muted text, lines, buttons and chips follow from the first three                                                                            |
| `colors`                                                         | any palette colour by name (below), applied last                                                                                                       |
| `corner_radius`, `border_width`, `padding`                       | pixels                                                                                                                                                 |
| `opacity`                                                        | 30 to 100 (percent; translucency needs a compositor)                                                                                                   |
| `headword_size`, `furigana_size`                                 | pixels; 0 follows the design                                                                                                                           |
| `font_family`                                                    | a font family; empty for Noto Sans CJK JP (on Linux with Noto Sans for Latin and Cyrillic)                                                             |
| `show_tags`, `show_dictionary`                                   | `true` or `false`                                                                                                                                      |
| `highlight_style`                                                | `underline`, `double-underline`, `dotted-underline`, `wavy-underline`, `outline`, `fill`, `brackets`                                                   |
| `highlight_color`                                                | `#rrggbbaa`; the alpha is how strongly a fill tints                                                                                                    |
| `highlight_thickness`, `highlight_radius`, `highlight_padding`   | pixels                                                                                                                                                 |
| `highlight_auto`                                                 | `true` picks the colour from the background under the text                                                                                             |

The palette colours `colors` can set: `background`, `text`, `muted`, `border`, `separator`, `scrollbar`, `accent`,
`on_accent`, `reading` (furigana), `chip`, `chip_text`, `button`, `button_icon`, `selection`, `frequency_value`,
`pill_text`, and the tag colours `tag_default`, `tag_name`, `tag_expression`, `tag_popular`, `tag_frequent`,
`tag_archaism`, `tag_dictionary`, `tag_frequency`, `tag_part_of_speech`, `tag_pitch`.
