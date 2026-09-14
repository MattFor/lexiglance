# Languages

Lexiglance reads Japanese, Russian, Ukrainian, Korean and Greek. It picks the language from the script of the text
under the pointer, so there is nothing to set up. Every installed dictionary is searched, whatever the language.

**Scanning -> Languages** turns off languages you do not read. Their text is then ignored. OCR models are only
downloaded and loaded for languages that are turned on and have dictionaries installed.

When languages share a script, like Russian and Ukrainian, the first one listed handles the text unless you turn it off.
Dictionaries of both are still searched.

## Adding a language

A language is a JSON file. You do not need to write code or rebuild anything.

- To use it yourself, put the file in the `languages` folder next to your dictionaries and restart Lexiglance
  (**Overview -> Restart**). **About -> Files -> Languages** opens the folder.
- To ship it with Lexiglance, add it to [data/languages](../data/languages) and open a pull request.

A file in your folder with the same code as a built-in language replaces it. If a file has a mistake, the log says what
it is and the file is skipped.

Example, Belarusian:

```json
{
  "code": "be",
  "name": "Belarusian",
  "script": [
    "0400-052F"
  ],
  "word_characters": "-'’",
  "sample_text": "У Іўі худы жвавы чорт у зялёнай камізэльцы пабег пад'есці фаршыраваных яек",
  "sample_words": [
    "мова",
    "хлеб"
  ],
  "commons_prefix": "Be",
  "ocr": {
    "paddle": "PP-OCRv5/rec/eslav_PP-OCRv5_rec_mobile.onnx",
    "tesseract": "bel"
  },
  "dictionaries": [
    {
      "name": "Wiktionary Belarusian-English",
      "category": "Terms",
      "title": "wty-be-en",
      "description": "Belarusian-English dictionary built from Wiktionary.",
      "homepage": "https://github.com/yomidevs/wiktionary-to-yomitan",
      "download": "https://huggingface.co/datasets/daxida/wty-release/resolve/main/latest/dict/be/en/wty-be-en.zip"
    }
  ]
}
```

| Field                   | Meaning                                                                                                                                                                                                                                               |
|-------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `code`, `name`          | The ISO 639-1 code and the English name. Required.                                                                                                                                                                                                    |
| `script`                | Unicode ranges, in hexadecimal, of the letters the language is written in. Text starting with one of them is looked up in this language. Required.                                                                                                    |
| `word_characters`       | Other characters that can be part of a word, such as hyphens and apostrophes.                                                                                                                                                                         |
| `folds`                 | Letters that are also searched as another letter, such as `{"ё": "е"}`. Use this for accents that are often left out.                                                                                                                                 |
| `sample_text`           | A line of text, used to check that a font can show the language.                                                                                                                                                                                      |
| `sample_words`          | Common words that any dictionary has, used by the health check and the previews.                                                                                                                                                                      |
| `commons_prefix`        | The prefix of the language's recordings on Wikimedia Commons, `Be` for `Be-мова.ogg`.                                                                                                                                                                 |
| `ocr`                   | `paddle` is a PaddleOCR recognition model from [RapidOCR](https://www.modelscope.cn/models/RapidAI/RapidOCR), needed for scripts other than Chinese, Japanese and Latin. `tesseract` and `tesseract_vertical` are Tesseract's names for the language. |
| `rules`, `minimum_stem` | Rules for inflected words, see below.                                                                                                                                                                                                                 |
| `dictionaries`          | What **Get recommended dictionaries** offers. `title`, or `title_prefix`, is the name the dictionary gives itself, so an installed one is recognised.                                                                                                 |

[wty](https://github.com/yomidevs/wiktionary-to-yomitan) has Wiktionary dictionaries for most languages. They list
every inflected form of a word, so most languages need no rules.

### Rules

```json
"minimum_stem": 2,
"rules": [
  {
    "inflected": "ами",
    "dictionary": "а",
    "class": "n",
    "form": "instrumental plural"
  }
]
```

A rule replaces the ending `inflected` with `dictionary`. It only matches dictionary entries with the part of speech
`class` (`n`, `v`, `adj`, or empty for any). The popup shows `form`. A rule with `after` applies to a word another rule
produced with that class. `minimum_stem` is how many letters must stay before the ending.

## Languages written in code

Japanese and Russian need more than a file can hold (kana, furigana, generated rules), so they are C++ classes in
[src/language](../src/language). Their files in [data/languages](../data/languages) only hold their dictionaries. A new
class derives from `lang::Language` or `lang::AlphabeticLanguage` in
[Language.h](../include/lexiglance/language/Language.h) and is added in `load()` in
[Language.cpp](../src/language/Language.cpp).
