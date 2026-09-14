#ifndef LEXIGLANCE_CONFIG_CONFIG_H
#define LEXIGLANCE_CONFIG_CONFIG_H

#include <lexiglance/core/Error.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::json
{
	class Value;
}

namespace lexiglance::config
{

	enum class SelectionMode : std::uint8_t
	{
		Off,
		Always,
		WithTrigger
	};

	enum class OcrMode : std::uint8_t
	{
		Off,
		Fallback,
		Always
	};

	enum class OcrEngine : std::uint8_t
	{
		Auto,
		Paddle,
		Tesseract
	};

	enum class Theme : std::uint8_t
	{
		Auto,
		Dark,
		Light
	};

	// Translucent overlays and rounded corners need a compositor; Auto follows whether one is running.
	enum class Compositor : std::uint8_t
	{
		Auto,
		On,
		Off,
	};

	enum class HighlightStyle : std::uint8_t
	{
		Underline,
		Outline,
		Fill,
		DoubleUnderline,
		DottedUnderline,
		WavyUnderline,
		Brackets
	};

	// The popup's layout: large furigana and plain words (Friendly), Yomitan's look (Classic), or dense (Compact).
	enum class PopupDesign : std::uint8_t
	{
		Friendly,
		Classic,
		Compact
	};

	// Colour schemes; each has a dark and a light variant, chosen by the theme.
	enum class ColorScheme : std::uint8_t
	{
		Default,
		Paper,
		Nord,
		Sakura,
		Matcha,
		Midnight,
		Contrast
	};

	// Where the popup opens: below the text (above it when there is no room there), or the other way round.
	enum class PopupPlacement : std::uint8_t
	{
		BelowText,
		AboveText
	};

	enum class MouseButton : std::uint8_t
	{
		Right,
		Middle
	};

	struct ScanSettings
	{
		std::vector<std::string> trigger{ "Super", "Alt_L" };
		int                      max_length        = 16;
		int                      delay_ms          = 20;
		int                      move_threshold    = 3;
		bool                     hide_on_no_result = false;
		bool                     search_kanji      = true;
		bool                     highlight         = true;
		bool                     accessibility     = true;
		SelectionMode            selection         = SelectionMode::Off;
		std::vector<std::string> ignored_windows;
		OcrMode                  ocr          = OcrMode::Fallback;
		bool                     ocr_vertical = false;
		std::string              ocr_model    = "fast";
		// PaddleOCR when its models are installed, Tesseract otherwise.
		OcrEngine ocr_engine = OcrEngine::Auto;
		// Only text starting in the script of a supported language is looked up (no popups for English interface text).
		bool known_languages_only = true;
		// While the trigger is held over a popup the wheel changes the looked-up length (instead of scrolling).
		bool                     wheel_length = true;
		std::vector<std::string> ocr_windows{ "steam_app_*", "*.exe*" };
	};

	// One colour of the popup's palette by its name ("muted", "tag_name", ...), set by a theme.
	struct NamedColor
	{
		std::string name;
		std::string value;
	};

	struct PopupSettings
	{
		int         width      = 400;
		int         max_height = 250;
		int         font_size  = 14;
		std::string font_family;
		double      scale       = 0.0;
		Theme       theme       = Theme::Auto;
		int         offset_x    = 0;
		int         offset_y    = 10;
		int         max_results = 32;
		// Term dictionaries shown: the first this many with a match, in priority order (0 for all).
		int         max_dictionaries = 0;
		bool        show_frequencies = true;
		bool        show_pitch       = true;
		bool        show_tags        = true;
		bool        show_furigana    = true;
		std::string highlight_color  = "#5aa0ff59";
		// Fill tints the matched text with the colour (its alpha is the strength); the others draw lines in it.
		HighlightStyle highlight_style     = HighlightStyle::Underline;
		Compositor     compositor          = Compositor::Auto;
		int            highlight_thickness = 2;
		// Pick the colour from the background under the text (the configured colour's opacity still applies).
		bool highlight_auto = false;
		// Dragging with this button selects popup text; releasing copies it.
		MouseButton select_button = MouseButton::Right;
		// Rounding of the highlight's corners and the room it leaves around the text (px).
		int highlight_radius  = 3;
		int highlight_padding = 2;

		PopupDesign    design    = PopupDesign::Friendly;
		ColorScheme    scheme    = ColorScheme::Default;
		PopupPlacement placement = PopupPlacement::BelowText;
		// Colours replacing the scheme's (#rrggbb); empty keeps the scheme's.
		std::string background_color;
		std::string text_color;
		std::string accent_color;
		std::string border_color;
		// Any other colour of the palette, applied last (themes written by hand can set every one).
		std::vector<NamedColor> colors;
		// The window: corner rounding (with a compositor), border and inner margin in px, background opacity in percent
		// (with a compositor).
		int corner_radius = 10;
		int border_width  = 1;
		int padding       = 12;
		int opacity       = 100;
		// Headword and furigana sizes in px; 0 follows the design (relative to the font size).
		int headword_size = 0;
		int furigana_size = 0;
		// What entries show besides their definitions.
		bool show_reading    = true; // the reading next to the headword when furigana is off
		bool show_inflection = true; // how a conjugated word was formed
		bool show_dictionary = true; // the names of the dictionaries
		bool show_buttons    = true; // audio and Anki buttons
		bool show_kanji      = true; // kanji entries when no word matches
		int  max_senses      = 0;    // definitions per dictionary of an entry; 0 shows all
	};

	struct AudioSettings
	{
		bool enabled  = true;
		bool autoplay = false;
		// Tried in order: "jpod101" (JapanesePod101, Japanese words), "commons" (Wikimedia Commons, many languages) or
		// URL templates with {term}, {reading} and {language}. Words no source applies to try Wikimedia Commons.
		std::vector<std::string> sources{ "jpod101", "commons" };
	};

	// {markers} available in Anki field templates.
	inline constexpr std::array<std::string_view, 17> anki_markers{
		"expression",
		"reading",
		"furigana",
		"furigana-plain",
		"glossary",
		"glossary-first",
		"sentence",
		"cloze-prefix",
		"cloze-body",
		"cloze-suffix",
		"audio",
		"frequencies",
		"pitch-accents",
		"pitch-accent-positions",
		"part-of-speech",
		"dictionary",
		"tags",
	};

	struct AnkiField
	{
		std::string name;
		std::string value;
	};

	struct AnkiSettings
	{
		bool                     enabled = false;
		std::string              url     = "http://127.0.0.1:8765";
		std::string              key;
		std::string              deck;
		std::string              model;
		std::vector<AnkiField>   fields;
		std::vector<std::string> tags{ "lexiglance" };
		bool                     allow_duplicates = false;
	};

	struct DictionaryPreference
	{
		std::string title;
		bool        enabled = true;
	};

	struct Config
	{
		// Text is looked up in the language of its script; this one takes text in a script several languages share (or in
		// none of theirs).
		std::string language = "ja";
		// Languages not looked up (codes), so text is only told apart among the others and OCR only reads those.
		std::vector<std::string>          disabled_languages;
		std::string                       log_level = "info";
		bool                              paused    = false;
		ScanSettings                      scan;
		PopupSettings                     popup;
		AudioSettings                     audio;
		AnkiSettings                      anki;
		std::vector<DictionaryPreference> dictionaries;

		// Missing or malformed fields keep their defaults; an invalid trigger is an error.
		[[nodiscard]] static Result<Config> fromJson( const json::Value& root );
		[[nodiscard]] static Result<Config> parse( std::string_view text );

		// A missing file yields the defaults.
		[[nodiscard]] static Result<Config> load( const std::filesystem::path& path );

		// Atomic replace, so readers never observe a partially written file.
		[[nodiscard]] Result<> save( const std::filesystem::path& path ) const;

		[[nodiscard]] std::string toJson( bool pretty = true ) const;

		[[nodiscard]] const DictionaryPreference* dictionary( std::string_view title ) const noexcept;
	};

} // namespace lexiglance::config

#endif // LEXIGLANCE_CONFIG_CONFIG_H
