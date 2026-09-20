#ifndef LEXIGLANCE_RENDER_HIGHLIGHT_H
#define LEXIGLANCE_RENDER_HIGHLIGHT_H

#include <lexiglance/render/Theme.h>

#include <cairo.h>

#include <cstdint>
#include <span>
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

	// Device pixels. The room around the text may be negative, to pull the mark in over a box that is reported wider
	// or taller than the text looks.
	struct HighlightLook
	{
		HighlightShape shape     = HighlightShape::Underline;
		int            thickness = 2;
		int            radius    = 3;
		int            padding_x = 2;
		int            padding_y = 2;

		[[nodiscard]] bool operator==( const HighlightLook& ) const = default;
	};

	struct Box
	{
		int x      = 0;
		int y      = 0;
		int width  = 0;
		int height = 0;
	};

	// The highlight's window around the matched text's box: the room around it, and room below the text for underlines.
	[[nodiscard]] Box highlightArea( const Box& text, const HighlightLook& look ) noexcept;

	// The rows an underline takes below the text box; 0 for the shapes drawn around it.
	[[nodiscard]] int highlightDepth( const HighlightLook& look ) noexcept;

	// Paints the highlight into a context the size of its window: lines opaque in `color`, a fill with its alpha.
	void drawHighlight( cairo_t* cr, int width, int height, const HighlightLook& look, const Color& color );

	// The pixels the highlight covers in a width x height window, as an X bitmap (rows of bytes, least significant bit
	// first): the window's shape where no compositor blends it.
	[[nodiscard]] std::vector<std::uint8_t> highlightMask( int width, int height, const HighlightLook& look );

	// A highlight in pieces, one for each line the text is on, in one window that spans them all: each piece is a box
	// in the window, drawn as drawHighlight() draws a window of its size.
	void                                    drawHighlight( cairo_t* cr, std::span<const Box> pieces, const HighlightLook& look, const Color& color );
	[[nodiscard]] std::vector<std::uint8_t> highlightMask( int width, int height, std::span<const Box> pieces, const HighlightLook& look );

	// Sample text with the highlight over its first word, on a light and on a dark background (for the settings), `width`
	// device pixels wide (the popup's, so the two previews line up; 0: as wide as the text needs). `automatic` picks the
	// colour for each background as on screen. The caller destroys the surface.
	[[nodiscard]] cairo_surface_t* highlightPreview( const HighlightLook& look, const Color& color, bool automatic, double scale, int width = 0 );

} // namespace lexiglance::render

#endif // LEXIGLANCE_RENDER_HIGHLIGHT_H
