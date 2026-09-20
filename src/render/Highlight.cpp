#include <lexiglance/render/Highlight.h>

#include <lexiglance/render/PopupRenderer.h>

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace lexiglance::render
{

	namespace
	{

		void roundedRect( cairo_t* cr, double x, double y, double w, double h, double r )
		{
			constexpr double pi = std::numbers::pi;
			r                   = std::clamp( r, 0.0, std::min( w, h ) / 2.0 );
			cairo_new_sub_path( cr );
			cairo_arc( cr, x + w - r, y + r, r, -pi / 2.0, 0.0 );
			cairo_arc( cr, x + w - r, y + h - r, r, 0.0, pi / 2.0 );
			cairo_arc( cr, x + r, y + h - r, r, pi / 2.0, pi );
			cairo_arc( cr, x + r, y + r, r, pi, 3.0 * pi / 2.0 );
			cairo_close_path( cr );
		}

		int thickness( const HighlightLook& look ) noexcept
		{
			return std::max( 1, look.thickness );
		}

		// A wavy line's height above and below its middle.
		double amplitude( const HighlightLook& look ) noexcept
		{
			return std::max( 2.0, static_cast<double>( thickness( look ) ) );
		}

		// A line along the bottom: rounded ends when the look is rounded.
		void bar( cairo_t* cr, double y, double w, double t, double radius )
		{
			roundedRect( cr, 0.0, y, w, t, std::min( radius, t / 2.0 ) );
			cairo_fill( cr );
		}

	} // namespace

	int highlightDepth( const HighlightLook& look ) noexcept
	{
		const int t = thickness( look );
		switch ( look.shape )
		{
			case HighlightShape::Underline:
				return t;
			case HighlightShape::DoubleUnderline:
				return 3 * t;
			case HighlightShape::DottedUnderline:
				return std::max( 2, t );
			case HighlightShape::WavyUnderline:
				return static_cast<int>( std::ceil( ( 2.0 * amplitude( look ) ) + t ) );
			case HighlightShape::Outline:
			case HighlightShape::Fill:
			case HighlightShape::Brackets:
				break;
		}
		return 0;
	}

	Box highlightArea( const Box& text, const HighlightLook& look ) noexcept
	{
		const int t = thickness( look );
		// Outlines and brackets are drawn just outside the room around the text, so they take some of their own.
		const int edge = look.shape == HighlightShape::Outline || look.shape == HighlightShape::Brackets ? t : 0;
		const int px   = look.padding_x + edge;
		const int py   = look.padding_y + edge;
		// Room taken away may not eat the box: there is always something to draw.
		const int width  = std::max( 2 * t, text.width + ( 2 * px ) );
		const int height = std::max( 2 * t, text.height + ( 2 * py ) + highlightDepth( look ) );
		return { .x = text.x - px, .y = text.y - py, .width = width, .height = height };
	}

	void drawHighlight( cairo_t* cr, int width, int height, const HighlightLook& look, const Color& color )
	{
		const double w = width;
		const double h = height;
		const double t = thickness( look );
		const double r = std::max( 0, look.radius );
		if ( w <= 0.0 || h <= 0.0 )
		{
			return;
		}
		cairo_save( cr );
		cairo_set_source_rgba( cr, color.r, color.g, color.b, look.shape == HighlightShape::Fill ? color.a : 1.0 );
		switch ( look.shape )
		{
			case HighlightShape::Fill:
				roundedRect( cr, 0.0, 0.0, w, h, r );
				cairo_fill( cr );
				break;
			case HighlightShape::Outline:
				roundedRect( cr, t / 2.0, t / 2.0, w - t, h - t, std::max( 0.0, r - ( t / 2.0 ) ) );
				cairo_set_line_width( cr, t );
				cairo_stroke( cr );
				break;
			case HighlightShape::Underline:
				bar( cr, h - t, w, t, r );
				break;
			case HighlightShape::DoubleUnderline:
				bar( cr, h - t, w, t, r );
				bar( cr, h - ( 3.0 * t ), w, t, r );
				break;
			case HighlightShape::DottedUnderline:
			{
				const double d = std::max( 2.0, t );
				for ( int i = 0; ( i * 2.0 * d ) + d <= w + 0.01; ++i )
				{
					const double x = i * 2.0 * d;
					if ( r > 0.0 )
					{
						cairo_new_sub_path( cr );
						cairo_arc( cr, x + ( d / 2.0 ), h - ( d / 2.0 ), d / 2.0, 0.0, 2.0 * std::numbers::pi );
					}
					else
					{
						cairo_rectangle( cr, x, h - d, d, d );
					}
				}
				cairo_fill( cr );
				break;
			}
			case HighlightShape::WavyUnderline:
			{
				const double a          = amplitude( look );
				const double wavelength = 4.0 * a;
				const double middle     = h - a - ( t / 2.0 );
				cairo_move_to( cr, 0.0, middle );
				for ( int i = 1; i <= w; ++i )
				{
					const double x = i;
					cairo_line_to( cr, x, middle + ( a * std::sin( 2.0 * std::numbers::pi * x / wavelength ) ) );
				}
				cairo_set_line_width( cr, t );
				cairo_set_line_join( cr, CAIRO_LINE_JOIN_ROUND );
				cairo_set_line_cap( cr, CAIRO_LINE_CAP_ROUND );
				cairo_stroke( cr );
				break;
			}
			case HighlightShape::Brackets:
			{
				// Corner marks, like a camera's focus frame.
				const double arm = std::clamp( std::min( w, h ) * 0.4, 2.0 * t, std::min( w, h ) / 2.0 );
				const double e   = t / 2.0;
				cairo_move_to( cr, e, arm );
				cairo_line_to( cr, e, e );
				cairo_line_to( cr, arm, e );
				cairo_move_to( cr, w - arm, e );
				cairo_line_to( cr, w - e, e );
				cairo_line_to( cr, w - e, arm );
				cairo_move_to( cr, w - e, h - arm );
				cairo_line_to( cr, w - e, h - e );
				cairo_line_to( cr, w - arm, h - e );
				cairo_move_to( cr, arm, h - e );
				cairo_line_to( cr, e, h - e );
				cairo_line_to( cr, e, h - arm );
				cairo_set_line_width( cr, t );
				cairo_set_line_cap( cr, r > 0.0 ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_SQUARE );
				cairo_set_line_join( cr, r > 0.0 ? CAIRO_LINE_JOIN_ROUND : CAIRO_LINE_JOIN_MITER );
				cairo_stroke( cr );
				break;
			}
		}
		cairo_restore( cr );
	}

	void drawHighlight( cairo_t* cr, std::span<const Box> pieces, const HighlightLook& look, const Color& color )
	{
		for ( const Box& piece : pieces )
		{
			cairo_save( cr );
			cairo_rectangle( cr, piece.x, piece.y, piece.width, piece.height );
			cairo_clip( cr );
			cairo_translate( cr, piece.x, piece.y );
			drawHighlight( cr, piece.width, piece.height, look, color );
			cairo_restore( cr );
		}
	}

	std::vector<std::uint8_t> highlightMask( int width, int height, const HighlightLook& look )
	{
		const Box whole{ .x = 0, .y = 0, .width = width, .height = height };
		return highlightMask( width, height, std::span( &whole, 1 ), look );
	}

	std::vector<std::uint8_t> highlightMask( int width, int height, std::span<const Box> pieces, const HighlightLook& look )
	{
		const int                 stride_bytes = ( std::max( 0, width ) + 7 ) / 8;
		std::vector<std::uint8_t> bits( static_cast<std::size_t>( stride_bytes ) * static_cast<std::size_t>( std::max( 0, height ) ), 0 );
		if ( width <= 0 || height <= 0 )
		{
			return bits;
		}
		cairo_surface_t* surface = cairo_image_surface_create( CAIRO_FORMAT_A8, width, height );
		cairo_t*         cr      = cairo_create( surface );
		// Crisp edges: a window shape has no partial pixels.
		cairo_set_antialias( cr, CAIRO_ANTIALIAS_NONE );
		drawHighlight( cr, pieces, look, Color{ .r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0 } );
		cairo_destroy( cr );
		cairo_surface_flush( surface );
		const unsigned char* data   = cairo_image_surface_get_data( surface );
		const int            stride = cairo_image_surface_get_stride( surface );
		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				if ( data[( static_cast<std::ptrdiff_t>( y ) * stride ) + x] >= 128 )
				{
					bits[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride_bytes ) ) + static_cast<std::size_t>( x / 8 )] |= static_cast<std::uint8_t>( 1U << static_cast<unsigned>( x % 8 ) );
				}
			}
		}
		cairo_surface_destroy( surface );
		return bits;
	}

	cairo_surface_t* highlightPreview( const HighlightLook& look, const Color& color, bool automatic, double scale, int width )
	{
		scale                    = std::clamp( scale, 0.5, 4.0 );
		const auto        px     = [scale]( double v ) { return static_cast<int>( std::lround( v * scale ) ); };
		const std::string sample = "日本語を勉強";
		const int         marked = static_cast<int>( std::string( "日本語" ).size() );
		const int         margin = px( 24 );

		// The text is laid out first: the panels fit around it, or it fits the panels when a width is asked for.
		// A font map of its own, freed with the preview (the shared default one lives as long as its thread).
		PangoFontMap*         map     = pango_cairo_font_map_new();
		PangoContext*         context = pango_font_map_create_context( map );
		PangoLayout*          layout  = pango_layout_new( context );
		PangoFontDescription* font    = pango_font_description_new();
		pango_font_description_set_family( font, fontFamilies().c_str() );
		pango_font_description_set_absolute_size( font, px( 22 ) * PANGO_SCALE );
		pango_layout_set_font_description( layout, font );
		pango_layout_set_text( layout, sample.c_str(), -1 );
		int sample_w = 0;
		int sample_h = 0;
		pango_layout_get_pixel_size( layout, &sample_w, &sample_h );
		if ( width > 0 && sample_w + ( 2 * margin ) > width / 2 )
		{
			// Too wide for half the width: a smaller size, not below 10 px.
			const int    half = width / 2;
			const double size = std::max( 10.0, px( 22 ) * static_cast<double>( half - ( 2 * margin ) ) / sample_w );
			pango_font_description_set_absolute_size( font, size * PANGO_SCALE );
			pango_layout_set_font_description( layout, font );
			pango_layout_get_pixel_size( layout, &sample_w, &sample_h );
		}
		pango_font_description_free( font );

		const int        total   = std::max( width, 2 * ( sample_w + ( 2 * margin ) ) );
		const int        panel_h = std::max( px( 64 ), sample_h + px( 36 ) );
		cairo_surface_t* surface = cairo_image_surface_create( CAIRO_FORMAT_ARGB32, total, panel_h );
		cairo_t*         cr      = cairo_create( surface );
		pango_cairo_update_layout( cr, layout );

		const HighlightLook scaled{ .shape = look.shape, .thickness = std::max( 1, px( look.thickness ) ), .radius = px( look.radius ), .padding_x = px( look.padding_x ), .padding_y = px( look.padding_y ) };
		for ( int panel = 0; panel < 2; ++panel )
		{
			const bool  dark       = panel == 1;
			const Color background = dark ? Color{ .r = 0.13, .g = 0.14, .b = 0.16 } : Color{ .r = 0.98, .g = 0.98, .b = 0.97 };
			const Color ink        = dark ? Color{ .r = 0.9, .g = 0.9, .b = 0.9 } : Color{ .r = 0.1, .g = 0.1, .b = 0.12 };
			const int   panel_w    = panel == 0 ? total / 2 : total - ( total / 2 );
			const int   x0         = panel * ( total / 2 );
			cairo_rectangle( cr, x0, 0.0, panel_w, panel_h );
			cairo_set_source_rgb( cr, background.r, background.g, background.b );
			cairo_fill( cr );

			int text_w = 0;
			int text_h = 0;
			pango_layout_get_pixel_size( layout, &text_w, &text_h );
			const double tx = x0 + ( ( panel_w - text_w ) / 2.0 );
			const double ty = ( panel_h - text_h ) / 2.0;
			cairo_move_to( cr, tx, ty );
			cairo_set_source_rgb( cr, ink.r, ink.g, ink.b );
			pango_cairo_show_layout( cr, layout );

			// The box of the marked word, as the capture reports it.
			PangoRectangle first{};
			PangoRectangle last{};
			pango_layout_index_to_pos( layout, 0, &first );
			const auto* end = g_utf8_prev_char( sample.c_str() + marked );
			pango_layout_index_to_pos( layout, static_cast<int>( end - sample.c_str() ), &last );
			const Box word{ .x      = static_cast<int>( tx + pango_units_to_double( first.x ) ),
				            .y      = static_cast<int>( ty + pango_units_to_double( first.y ) ),
				            .width  = static_cast<int>( pango_units_to_double( last.x + last.width - first.x ) ),
				            .height = static_cast<int>( pango_units_to_double( first.height ) ) };
			const Box area = highlightArea( word, scaled );
			Color     mark = color;
			if ( automatic )
			{
				const auto                       byte  = []( double v ) { return static_cast<std::uint32_t>( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) ); };
				const auto                       pixel = ( byte( background.r ) << 16U ) | ( byte( background.g ) << 8U ) | byte( background.b );
				const std::vector<std::uint32_t> pixels( 64, pixel );
				mark = autoHighlight( pixels, color.a, look.shape == HighlightShape::Fill );
			}
			cairo_save( cr );
			cairo_translate( cr, area.x, area.y );
			drawHighlight( cr, area.width, area.height, scaled, mark );
			cairo_restore( cr );
		}
		g_object_unref( layout );
		g_object_unref( context );
		g_object_unref( map );
		cairo_destroy( cr );
		cairo_surface_flush( surface );
		return surface;
	}

} // namespace lexiglance::render
