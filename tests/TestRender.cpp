#include "Test.h"

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/Translator.h>
#include <lexiglance/render/PopupRenderer.h>
#include <lexiglance/render/Theme.h>

#include <cairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	std::vector<std::uint32_t> filled( std::uint32_t pixel, std::size_t count = 400 )
	{
		return std::vector<std::uint32_t>( count, pixel );
	}

	const test::Registrar auto_highlight( "automatic highlight colour", [] {
		// Neutral light backgrounds get blue, dark ones amber, coloured ones the opposite hue.
		const auto light = lg::render::autoHighlight( filled( 0xFFFFFF ), 0.4, true );
		test::expect( light.b > light.r );
		test::expectEqual( light.a, 0.4 );
		const auto dark = lg::render::autoHighlight( filled( 0x101010 ), 0.4, true );
		test::expect( dark.r > dark.b );
		const auto blue = lg::render::autoHighlight( filled( 0x2040C0 ), 1.0, false );
		test::expect( blue.r > blue.b );
		test::expectEqual( blue.a, 1.0 );

		// Black text on a light page: the background decides, not the ink.
		auto page = filled( 0xF4F4F4, 300 );
		page.insert( page.end(), 100, 0x000000 );
		test::expect( lg::render::autoHighlight( page, 0.35, true ).b > 0.5 );

		// An opaque fill would hide the text, so it falls back to a tint.
		test::expect( lg::render::autoHighlight( filled( 0xFFFFFF ), 1.0, true ).a < 0.5 );
	} );

	lg::render::PopupImage::Glyph glyph( float x, float y, std::uint32_t begin, std::uint32_t end )
	{
		return { .x = x, .y = y, .width = 14.0F, .height = 16.0F, .begin = begin, .end = end };
	}

	const test::Registrar popup_selection( "popup text selection", [] {
		// "食べる" and a separate "n" tag on one line, "to eat" on the next.
		const std::string                                text = "食べるnto eat";
		const std::vector<lg::render::PopupImage::Glyph> glyphs{
			glyph( 0, 0, 0, 3 ),
			glyph( 14, 0, 3, 6 ),
			glyph( 28, 0, 6, 9 ),
			glyph( 60, 0, 9, 10 ),
			glyph( 0, 30, 10, 11 ),
			glyph( 14, 30, 11, 12 ),
			glyph( 28, 30, 12, 13 ),
			glyph( 42, 30, 13, 14 ),
			glyph( 56, 30, 14, 15 ),
			glyph( 70, 30, 15, 16 ),
		};
		const lg::render::PopupImage image( cairo_image_surface_create( CAIRO_FORMAT_ARGB32, 1, 1 ), 100, 50, {}, {}, text, glyphs );

		test::expectEqual( image.selectedText( 0, 9 ), std::string( "食べる n\nto eat" ) );
		test::expectEqual( image.selectedText( 2, 1 ), std::string( "べる" ) );
		test::expectEqual( image.glyphAt( 16, 5 ).value_or( 99 ), std::size_t{ 1 } );
		// Far right of the second line: the nearest glyph on that line, not one above it.
		test::expectEqual( image.glyphAt( 200, 36 ).value_or( 99 ), std::size_t{ 9 } );
	} );

	const test::Registrar highlighter_marker( "highlighter marker", [] {
		// A dark 4x4 glyph on white: the background is covered, the glyph and one pixel around it are not.
		constexpr int              width  = 20;
		constexpr int              height = 10;
		std::vector<std::uint32_t> pixels( static_cast<std::size_t>( width ) * height, 0xFFFFFFFFU );
		for ( int y = 3; y < 7; ++y )
		{
			for ( int x = 8; x < 12; ++x )
			{
				pixels[( y * width ) + x] = 0xFF101010U;
			}
		}
		const auto mark = lg::render::marker( pixels, width, height );
		test::expect( mark.usable );
		test::expect( mark.background.r > 0.95 && mark.background.b > 0.95 );
		test::expectEqual( mark.cover.size(), pixels.size() );
		if ( mark.cover.size() != pixels.size() )
		{
			return;
		}
		test::expectEqual( static_cast<int>( mark.cover[0] ), 1 );
		test::expectEqual( static_cast<int>( mark.cover[( 4 * width ) + 9] ), 0 );
		test::expectEqual( static_cast<int>( mark.cover[( 4 * width ) + 7] ), 0 );
		test::expectEqual( static_cast<int>( mark.cover[( 4 * width ) + 5] ), 1 );

		// A checkerboard has no background colour to mark.
		std::vector<std::uint32_t> checker( static_cast<std::size_t>( width ) * height );
		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				checker[( y * width ) + x] = ( ( x + y ) % 2 ) == 0 ? 0xFFFFFFFFU : 0xFF000000U;
			}
		}
		test::expect( !lg::render::marker( checker, width, height ).usable );
	} );

	std::shared_ptr<const lg::lookup::DictionarySet> fixtureSet()
	{
		const auto output = test::scratch( "render" ) / "fixture.lgd";
		if ( !lg::dict::compile( test::fixtureDirectory(), output ) )
		{
			return nullptr;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		if ( !dictionary )
		{
			return nullptr;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.push_back( { .dictionary = *dictionary, .styles = nullptr, .name = "Fixture" } );
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	const test::Registrar selection_header( "a wheel selection longer than any entry is shown in the popup", [] {
		const auto set = fixtureSet();
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator    translator;
		lg::lookup::LookupOptions options;
		options.max_length = 5;
		options.selection  = true;
		const auto result  = translator.lookup( set, "食べるもの", options );
		test::expectEqual( result.selected, std::uint32_t{ 5 } );
		test::expectEqual( result.matched_length, std::uint32_t{ 3 } );

		lg::render::PopupRenderer renderer;
		const auto                image = renderer.render( result );
		if ( !test::expect( image != nullptr ) )
		{
			return;
		}
		// The selection comes first and clicking it copies it; the entry follows.
		const auto* top = image->regionAt( 1 );
		if ( !test::expect( top != nullptr ) )
		{
			return;
		}
		test::expectEqual( top->text, std::string( "食べるもの" ) );
		const auto* entry = image->regionAt( top->bottom + 1 );
		test::expect( entry != nullptr && entry->text == "食べる" );
	} );

} // namespace

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	const test::Registrar popup_translation( "popup with a translation", [] {
		lg::render::PopupRenderer      renderer;
		const lg::lookup::LookupResult nothing;
		// A sentence no dictionary has a word of still gets a popup for its translation.
		test::expect( renderer.render( nothing ) == nullptr );
		const lg::render::Translation waiting{ .source = "Я читаю книгу.", .text = {}, .problem = {} };
		const auto                    first = renderer.render( nothing, {}, 0, &waiting );
		if ( !test::expect( first != nullptr ) )
		{
			return;
		}
		// Not an entry: middle-click finds no entry there to play, a click copies the text.
		test::expect( !first->entryAt( 5 ).has_value() );
		test::expect( first->regionAt( 5 ) != nullptr && first->regionAt( 5 )->text == "Я читаю книгу." );

		const lg::render::Translation done{ .source = "Я читаю книгу.", .text = "I am reading a book.", .problem = {} };
		const auto                    second = renderer.render( nothing, {}, 0, &done );
		test::expect( second != nullptr && second->regionAt( 5 ) != nullptr && second->regionAt( 5 )->text == "I am reading a book." );
	} );

	const test::Registrar popup_regions( "popup regions count entries only", [] {
		const std::vector<lg::render::PopupImage::Region> regions{
			{ .top = 0, .bottom = 20, .text = "translation", .entry = false },
			{ .top = 20, .bottom = 40, .text = "first", .entry = true },
			{ .top = 40, .bottom = 60, .text = "second", .entry = true },
		};
		const lg::render::PopupImage image( cairo_image_surface_create( CAIRO_FORMAT_ARGB32, 1, 1 ), 100, 60, regions );
		test::expect( !image.entryAt( 10 ).has_value() );
		test::expectEqual( image.entryAt( 30 ).value_or( 99 ), std::size_t{ 0 } );
		test::expectEqual( image.entryAt( 50 ).value_or( 99 ), std::size_t{ 1 } );
		test::expectEqual( image.regionAt( 10 )->text, std::string( "translation" ) );
	} );

} // namespace
