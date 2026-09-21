#ifndef LEXIGLANCE_RENDER_THEME_H
#define LEXIGLANCE_RENDER_THEME_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::render
{

	struct Color
	{
		double r = 0.0;
		double g = 0.0;
		double b = 0.0;
		double a = 1.0;

		// #rgb, #rrggbb or #rrggbbaa.
		[[nodiscard]] static std::optional<Color> parse( std::string_view text ) noexcept;

		[[nodiscard]] std::string hex() const;

		[[nodiscard]] bool operator==( const Color& ) const = default;
	};

	// Colour schemes, each with a dark and a light variant.
	enum class Scheme : std::uint8_t
	{
		Default,
		Paper,
		Nord,
		Sakura,
		Matcha,
		Midnight,
		Contrast
	};

	// Popup palette; tag colours follow Yomitan's categories so dictionaries look the way their authors intended.
	struct Theme
	{
		Color background;
		Color border;
		Color text;
		Color muted;
		Color separator;
		Color scrollbar;
		Color pill_text;
		Color tag_default;
		Color tag_name;
		Color tag_expression;
		Color tag_popular;
		Color tag_frequent;
		Color tag_archaism;
		Color tag_dictionary;
		Color tag_frequency;
		Color tag_part_of_speech;
		Color tag_pitch;
		Color frequency_value;
		Color button;
		Color button_icon;
		Color selection;
		// The scheme's own colour: furigana, sense numbers, dictionary captions; what goes on it; soft chips.
		Color accent;
		Color on_accent;
		Color reading;
		Color chip;
		Color chip_text;

		[[nodiscard]] static Theme dark();
		[[nodiscard]] static Theme light();

		// A scheme's dark or light variant.
		[[nodiscard]] static Theme make( Scheme scheme, bool dark );

		// With some colours replaced; those that follow from them (muted text, lines, buttons) are derived anew.
		[[nodiscard]] Theme withColors( std::optional<Color> background, std::optional<Color> text, std::optional<Color> accent, std::optional<Color> border ) const;

		// Sets one colour by its name in theme files ("muted", "tag_name", ...); false for an unknown name.
		bool set( std::string_view name, const Color& color );

		// The names set() knows.
		[[nodiscard]] static std::vector<std::string_view> colorNames();

		[[nodiscard]] Color tag( std::string_view category ) const noexcept;

		[[nodiscard]] bool operator==( const Theme& ) const = default;
	};

	// Linear blend: `t` = 0 gives `a`, 1 gives `b`.
	[[nodiscard]] Color mix( const Color& a, const Color& b, double t ) noexcept;

	// WCAG contrast ratio of two opaque colours (1 to 21); text wants at least 4.5.
	[[nodiscard]] double contrast( const Color& a, const Color& b ) noexcept;

	// A highlighter's fill goes over the text, so its colour's alpha is the strength of the tint. An alpha this high is
	// no strength at all: such a fill would hide the very word it marks, so it stands for "as strong as looks right" and
	// becomes `fill_tint` instead.
	constexpr double opaque_fill = 0.95;
	constexpr double fill_tint   = 0.35;

	// A highlight colour that stands out on the background under the text (XRGB pixels): blue on light backgrounds,
	// amber on dark ones, the opposite hue on coloured ones. Fills keep `opacity`; lines are opaque.
	[[nodiscard]] Color autoHighlight( std::span<const std::uint32_t> pixels, double opacity, bool fill ) noexcept;

	// A highlighter mark over text on a plain background: which pixels of a width x height block to cover (the
	// background around the glyphs, row by row) and that background's colour. Not usable on textured backgrounds.
	struct Marker
	{
		bool                      usable = false;
		Color                     background;
		std::vector<std::uint8_t> cover;
	};

	[[nodiscard]] Marker marker( std::span<const std::uint32_t> pixels, int width, int height );

} // namespace lexiglance::render

#endif // LEXIGLANCE_RENDER_THEME_H
