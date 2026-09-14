#ifndef LEXIGLANCE_RENDER_HIGHLIGHT_H
#define LEXIGLANCE_RENDER_HIGHLIGHT_H

#include <lexiglance/render/Theme.h>

#include <cairo.h>

#include <cstdint>
#include <vector>

// The mark over the text being looked up: its shapes, drawn the same way on screen, in window shapes and in previews.
namespace lexiglance::render
{

	enum class HighlightShape : std::uint8_t
	{
		Underline,
		Outline,
		Fill,
		DoubleUnderline,
		DottedUnderline,
		WavyUnderline,
		Brackets
	};

	// Device pixels.
	struct HighlightLook
	{
		HighlightShape shape     = HighlightShape::Underline;
		int            thickness = 2;
		int            radius    = 3;
		int            padding   = 2;

		[[nodiscard]] bool operator==( const HighlightLook& ) const = default;
	};

	struct Box
	{
		int x      = 0;
		int y      = 0;
		int width  = 0;
		int height = 0;
	};

	// The highlight's window around the matched text's box: the padding, and room below the text for underlines.
	[[nodiscard]] Box highlightArea( const Box& text, const HighlightLook& look ) noexcept;

	// Paints the highlight into a context the size of its window: lines opaque in `color`, a fill with its alpha.
	void drawHighlight( cairo_t* cr, int width, int height, const HighlightLook& look, const Color& color );

	// The pixels the highlight covers in a width x height window, as an X bitmap (rows of bytes, least significant bit
	// first): the window's shape where no compositor blends it.
	[[nodiscard]] std::vector<std::uint8_t> highlightMask( int width, int height, const HighlightLook& look );

	// Sample text with the highlight over its first word, on a light and on a dark background (for the settings), `width`
	// device pixels wide (the popup's, so the two previews line up; 0: as wide as the text needs). `automatic` picks the
	// colour for each background as on screen. The caller destroys the surface.
	[[nodiscard]] cairo_surface_t* highlightPreview( const HighlightLook& look, const Color& color, bool automatic, double scale, int width = 0 );

} // namespace lexiglance::render

#endif // LEXIGLANCE_RENDER_HIGHLIGHT_H
