#include "Test.h"

#include <lexiglance/ocr/Paddle.h>

#include <string>
#include <vector>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	const test::Registrar ocr_text_boxes( "ocr detection boxes", [] {
		// A 40x10 map with one confident 20x4 region and one faint blob.
		constexpr int      width  = 40;
		constexpr int      height = 10;
		std::vector<float> map( static_cast<std::size_t>( width ) * height, 0.0F );
		for ( int y = 3; y < 7; ++y )
		{
			for ( int x = 5; x < 25; ++x )
			{
				map[( y * width ) + x] = 0.9F;
			}
		}
		for ( int y = 2; y < 6; ++y )
		{
			for ( int x = 30; x < 34; ++x )
			{
				map[( y * width ) + x] = 0.5F;
			}
		}
		const auto boxes = lg::ocr::textBoxes( map, width, height );
		test::expectEqual( boxes.size(), std::size_t{ 1 } );
		if ( boxes.empty() )
		{
			return;
		}
		// The inner box is the region itself (grown by the 2x2 dilation); the outer one adds the recognition margin.
		test::expectEqual( boxes.front().inner_x, 5 );
		test::expectEqual( boxes.front().inner_y, 3 );
		test::expect( boxes.front().width > boxes.front().inner_width );
		test::expect( boxes.front().score > 0.5F );

		// The looser pass (sparse glyphs) finds the faint blob too.
		const auto loose = lg::ocr::textBoxes( map, width, height, 0.15F, 0.3F );
		test::expectEqual( loose.size(), std::size_t{ 2 } );
	} );

	const test::Registrar ocr_ctc( "ocr ctc decoding", [] {
		// Classes: blank, 猫, で, space. Steps: 猫 猫 blank 猫 で で blank.
		const std::vector<std::string> dictionary{ "猫", "で" };
		const std::vector<int>         path{ 1, 1, 0, 1, 2, 2, 0 };
		std::vector<float>             probabilities;
		for ( const int best : path )
		{
			for ( int c = 0; c < 4; ++c )
			{
				probabilities.push_back( c == best ? 0.8F : 0.2F / 3.0F );
			}
		}
		const auto decoded = lg::ocr::decodeCtc( probabilities, static_cast<int>( path.size() ), 4, dictionary );
		test::expectEqual( decoded.size(), std::size_t{ 3 } );
		if ( decoded.size() != 3 )
		{
			return;
		}
		// A repeat merges; a blank between two equal classes separates them.
		test::expectEqual( decoded[0].text, std::string( "猫" ) );
		test::expectEqual( decoded[0].first, 0 );
		test::expectEqual( decoded[0].last, 1 );
		test::expectEqual( decoded[1].text, std::string( "猫" ) );
		test::expectEqual( decoded[2].text, std::string( "で" ) );
		test::expectEqual( decoded[2].last, 5 );
		test::expect( decoded[2].confidence > 0.79F && decoded[2].confidence < 0.81F );
	} );

} // namespace
