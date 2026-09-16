#include "Test.h"

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/Translator.h>
#include <lexiglance/render/Highlight.h>
#include <lexiglance/render/PopupRenderer.h>
#include <lexiglance/render/Theme.h>

#include <cairo.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;
	using lg::render::Color;
	using lg::render::HighlightShape;
	using lg::render::Theme;

	constexpr std::array schemes{ lg::render::Scheme::Default, lg::render::Scheme::Paper, lg::render::Scheme::Nord, lg::render::Scheme::Sakura, lg::render::Scheme::Matcha, lg::render::Scheme::Midnight, lg::render::Scheme::Contrast };

	const test::Registrar scheme_contrast( "every colour scheme is readable, dark and light", [] {
		for ( const auto scheme : schemes )
		{
			for ( const bool dark : { true, false } )
			{
				const Theme theme = Theme::make( scheme, dark );
				// Text at least WCAG AA, the high contrast scheme AAA; furigana, captions and quiet text large-text AA.
				test::expect( lg::render::contrast( theme.text, theme.background ) >= ( scheme == lg::render::Scheme::Contrast ? 7.0 : 4.5 ) );
				test::expect( lg::render::contrast( theme.reading, theme.background ) >= 3.0 );
				test::expect( lg::render::contrast( theme.accent, theme.background ) >= 3.0 );
				test::expect( lg::render::contrast( theme.muted, theme.background ) >= 3.0 );
				test::expect( lg::render::contrast( theme.chip_text, theme.chip ) >= 3.0 );
				const double luminance = ( theme.background.r + theme.background.g + theme.background.b ) / 3.0;
				test::expect( dark ? luminance < 0.3 : luminance > 0.7 );
			}
		}
		test::expect( Theme::make( lg::render::Scheme::Default, true ) == Theme::dark() );
		test::expect( Theme::make( lg::render::Scheme::Default, false ) == Theme::light() );
	} );

	const test::Registrar theme_colours( "own colours replace a scheme's", [] {
		const Theme base = Theme::dark();
		// A new background brings muted text and lines along with it.
		const Theme paper = base.withColors( Color{ .r = 0.96, .g = 0.94, .b = 0.88 }, Color{ .r = 0.1, .g = 0.1, .b = 0.1 }, std::nullopt, std::nullopt );
		test::expect( paper.background.r > 0.9 );
		test::expect( paper.muted != base.muted );
		test::expect( lg::render::contrast( paper.muted, paper.background ) >= 3.0 );
		// An accent alone keeps the rest.
		const Theme accented = base.withColors( std::nullopt, std::nullopt, Color{ .r = 1.0, .g = 0.5, .b = 0.0 }, std::nullopt );
		test::expect( accented.background == base.background && accented.text == base.text );
		test::expect( accented.accent.r > 0.99 && accented.reading != base.reading );
		test::expect( base.withColors( std::nullopt, std::nullopt, std::nullopt, std::nullopt ) == base );
		// Any colour by name, as theme files set them.
		Theme named = base;
		test::expect( named.set( "tag_name", Color{ .r = 0.0, .g = 1.0, .b = 0.0 } ) );
		test::expect( named.tag_name.g > 0.99 );
		test::expect( !named.set( "no_such_colour", Color{} ) );
		const auto names = Theme::colorNames();
		test::expect( std::ranges::contains( names, std::string_view( "reading" ) ) && std::ranges::contains( names, std::string_view( "background" ) ) );
	} );

	// A dictionary of one word with five senses, compiled into the scratch directory.
	std::shared_ptr<const lg::lookup::DictionarySet> senses()
	{
		const auto source = test::scratch( "senses-source" );
		std::ofstream( source / "index.json" ) << R"json({"title": "Senses", "revision": "1", "format": 3})json";
		std::ofstream( source / "tag_bank_1.json" ) << R"json([["n", "partOfSpeech", 0, "noun (common) (futsuumeishi)", 0]])json";
		std::ofstream( source / "term_bank_1.json" ) << R"json([
			["猫", "ねこ", "n", "", 5, ["cat"], 1, ""],
			["猫", "ねこ", "n", "", 4, ["shamisen"], 1, ""],
			["猫", "ねこ", "n", "", 3, ["geisha"], 1, ""],
			["猫", "ねこ", "n", "", 2, ["wheelbarrow"], 1, ""],
			["猫", "ねこ", "n", "", 1, ["clay bed-warmer"], 1, ""]
		])json";
		const auto output = test::scratch( "senses" ) / "senses.lgd";
		if ( !lg::dict::compile( source, output ) )
		{
			return nullptr;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		if ( !dictionary )
		{
			return nullptr;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.push_back( { .dictionary = *dictionary, .styles = nullptr, .name = "Senses" } );
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	std::string allText( const lg::render::PopupImage& image )
	{
		return image.glyphs().empty() ? std::string() : image.selectedText( 0, image.glyphs().size() - 1 );
	}

	const test::Registrar popup_designs( "friendly, classic and compact popups", [] {
		const auto set = senses();
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		const auto             result = translator.lookup( set, "猫" );
		if ( !test::expect( result.terms.size() == 1 && result.terms.front().definitions.size() == 5 ) )
		{
			return;
		}

		lg::render::PopupRenderer renderer;
		const auto                render = [&]( lg::render::Design design, int max_senses = 0 ) {
			lg::render::PopupStyle style;
			style.design     = design;
			style.max_senses = max_senses;
			renderer.setStyle( style );
			return renderer.render( result );
		};
		const auto friendly = render( lg::render::Design::Friendly );
		const auto classic  = render( lg::render::Design::Classic );
		const auto compact  = render( lg::render::Design::Compact );
		if ( !test::expect( friendly && classic && compact ) )
		{
			return;
		}
		// Every sense is there in each design, and the headword copies as it is.
		for ( const auto& image : { friendly, classic, compact } )
		{
			const auto text = allText( *image );
			test::expect( text.contains( "猫" ) && text.contains( "cat" ) && text.contains( "clay bed-warmer" ) );
			test::expect( image->regionAt( 1 ) != nullptr && image->regionAt( 1 )->text == "猫" );
		}
		// The friendly design is the roomy one, compact the dense one.
		test::expect( friendly->height() > compact->height() );

		// A limit on the senses keeps the first ones and says how many more there are.
		const auto limited = render( lg::render::Design::Friendly, 2 );
		if ( test::expect( limited != nullptr ) )
		{
			const auto text = allText( *limited );
			test::expect( text.contains( "cat" ) && text.contains( "shamisen" ) );
			test::expect( !text.contains( "geisha" ) && !text.contains( "clay bed-warmer" ) );
			test::expect( limited->height() < friendly->height() );
		}
	} );

	const test::Registrar kanji_entries( "kanji entries can be left out", [] {
		const auto output = test::scratch( "kanji-render" ) / "fixture.lgd";
		if ( !test::expect( lg::dict::compile( test::fixtureDirectory(), output ).has_value() ) )
		{
			return;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		if ( !test::expect( dictionary.has_value() ) || ( *dictionary )->kanji().empty() )
		{
			return;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.push_back( { .dictionary = *dictionary, .styles = nullptr, .name = "Fixture" } );
		const auto set = std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );

		// A kanji the dictionary has that no word starts with.
		lg::lookup::Translator translator;
		std::string            character;
		for ( const auto& record : ( *dictionary )->kanji() )
		{
			const auto candidate = std::string( ( *dictionary )->string( record.character ) );
			if ( translator.lookup( set, candidate ).terms.empty() )
			{
				character = candidate;
				break;
			}
		}
		if ( character.empty() )
		{
			return;
		}
		const auto result = translator.lookup( set, character );
		test::expect( result.terms.empty() && !result.kanji.empty() );

		lg::render::PopupRenderer renderer;
		lg::render::PopupStyle    style;
		renderer.setStyle( style );
		test::expect( renderer.render( result ) != nullptr );
		style.show_kanji = false;
		renderer.setStyle( style );
		test::expect( renderer.render( result ) == nullptr );
	} );

	// Whether pixel (x, y) of an X bitmap is set.
	bool bit( const std::vector<std::uint8_t>& mask, int width, int x, int y )
	{
		const int stride = ( width + 7 ) / 8;
		return ( mask[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride ) ) + static_cast<std::size_t>( x / 8 )] & ( 1U << static_cast<unsigned>( x % 8 ) ) ) != 0;
	}

	const test::Registrar highlight_shapes( "highlight shapes", [] {
		using lg::render::highlightArea;
		using lg::render::highlightMask;
		const lg::render::Box text{ .x = 100, .y = 50, .width = 60, .height = 20 };

		// The window: the room around the text, and room below it for lines.
		const auto fill = highlightArea( text, { .shape = HighlightShape::Fill, .thickness = 2, .radius = 4, .padding_x = 3, .padding_y = 3 } );
		test::expect( fill.x == 97 && fill.y == 47 && fill.width == 66 && fill.height == 26 );
		const auto outline = highlightArea( text, { .shape = HighlightShape::Outline, .thickness = 2, .radius = 0, .padding_x = 1, .padding_y = 1 } );
		test::expect( outline.x == 97 && outline.width == 66 && outline.height == 26 );
		const auto underline = highlightArea( text, { .shape = HighlightShape::Underline, .thickness = 2, .radius = 0, .padding_x = 0, .padding_y = 0 } );
		test::expect( underline.y == 50 && underline.height == 22 );
		// Room sideways and above are given apart, and either can be taken away again.
		const auto wide = highlightArea( text, { .shape = HighlightShape::Fill, .thickness = 2, .radius = 0, .padding_x = 6, .padding_y = -2 } );
		test::expect( wide.x == 94 && wide.width == 72 && wide.y == 52 && wide.height == 16 );
		// Never past the box itself: a short word keeps something to draw.
		const lg::render::Box small{ .x = 10, .y = 10, .width = 8, .height = 8 };
		const auto            pinched = highlightArea( small, { .shape = HighlightShape::Fill, .thickness = 2, .radius = 0, .padding_x = -16, .padding_y = -16 } );
		test::expect( pinched.width == 4 && pinched.height == 4 );
		test::expect( highlightArea( text, { .shape = HighlightShape::DoubleUnderline, .thickness = 2 } ).height > underline.height );
		test::expect( highlightArea( text, { .shape = HighlightShape::WavyUnderline, .thickness = 2 } ).height > underline.height );

		constexpr int w = 60;
		constexpr int h = 30;
		// Underlines cover the bottom only; the text above stays visible.
		for ( const auto shape : { HighlightShape::Underline, HighlightShape::DoubleUnderline, HighlightShape::DottedUnderline, HighlightShape::WavyUnderline } )
		{
			const auto mask = highlightMask( w, h, { .shape = shape, .thickness = 2, .radius = 0, .padding_x = 0, .padding_y = 0 } );
			bool       top  = false;
			bool       low  = false;
			for ( int x = 0; x < w; ++x )
			{
				for ( int y = 0; y < h / 2; ++y )
				{
					top = top || bit( mask, w, x, y );
				}
				low = low || bit( mask, w, x, h - 1 ) || bit( mask, w, x, h - 2 ) || bit( mask, w, x, h - 3 );
			}
			test::expect( !top && low );
		}
		// An outline is hollow; rounded corners leave the very corner out.
		const auto square = highlightMask( w, h, { .shape = HighlightShape::Outline, .thickness = 2, .radius = 0 } );
		test::expect( bit( square, w, 0, 0 ) && bit( square, w, w / 2, 0 ) && !bit( square, w, w / 2, h / 2 ) );
		const auto rounded = highlightMask( w, h, { .shape = HighlightShape::Fill, .thickness = 2, .radius = 8 } );
		test::expect( !bit( rounded, w, 0, 0 ) && bit( rounded, w, w / 2, h / 2 ) && bit( rounded, w, w / 2, 0 ) );
		// Brackets mark the corners only.
		const auto brackets = highlightMask( w, h, { .shape = HighlightShape::Brackets, .thickness = 2, .radius = 0 } );
		test::expect( bit( brackets, w, 1, 1 ) && bit( brackets, w, w - 2, h - 2 ) && !bit( brackets, w, w / 2, 1 ) && !bit( brackets, w, w / 2, h / 2 ) );
		test::expect( highlightMask( 0, 0, {} ).empty() );

		// The preview: two panels with the sample text and the mark.
		cairo_surface_t* preview = lg::render::highlightPreview( { .shape = HighlightShape::Fill, .thickness = 2, .radius = 4, .padding_x = 2, .padding_y = 2 }, Color{ .r = 1.0, .g = 0.8, .b = 0.0, .a = 0.5 }, false, 1.0 );
		test::expect( cairo_image_surface_get_width( preview ) > 2 * cairo_image_surface_get_height( preview ) );
		cairo_surface_destroy( preview );
	} );

	const test::Registrar text_raster( "text drawn for the OCR self-test", [] {
		const auto raster = lg::render::rasterizeText( "日本語", 32.0 );
		if ( !test::expect( raster.width > 0 && raster.height > 0 && raster.pixels.size() == static_cast<std::size_t>( raster.width ) * static_cast<std::size_t>( raster.height ) ) )
		{
			return;
		}
		// White paper with dark ink on it.
		test::expectEqual( raster.pixels.front(), std::uint32_t{ 0xFFFFFF } );
		test::expect( std::ranges::any_of( raster.pixels, []( std::uint32_t pixel ) { return ( pixel & 0xFFU ) < 0x40U; } ) );
		test::expect( !lg::render::checkFont( "abc" ).family.empty() );
	} );

} // namespace
