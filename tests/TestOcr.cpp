#include "Test.h"

#include <lexiglance/ocr/Onnx.h>
#include <lexiglance/ocr/Paddle.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <ios>
#include <string>
#include <utility>
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

	const test::Registrar ocr_not_set_up( "ocr not set up is not a fault", [] {
		// Neither state is worth reporting over a fallback engine's own trouble, so both come from the real load: a
		// literal here would pass while the message the daemon shows drifted away from it.
		const auto nothing = test::scratch( "ocr-nothing" );
		const auto absent  = lg::ocr::PaddleOcr::load( nothing, nothing, 1 );
		test::expect( !absent.has_value() );
		if ( !absent.has_value() )
		{
			test::expect( !lg::ocr::loadFailureActionable( absent.error().message ) );
		}

		// Models without a runtime to run them: as far as the models go this is set up, so the runtime is what answers.
		const auto models = test::scratch( "ocr-no-runtime" );
		std::ofstream( models / "det.onnx", std::ios::binary ) << "not a model";
		const auto runtimeless = lg::ocr::PaddleOcr::load( models, nothing, 1 );
		test::expect( !runtimeless.has_value() );
		if ( !runtimeless.has_value() )
		{
			test::expect( !lg::ocr::loadFailureActionable( runtimeless.error().message ) );
		}

		// Anything else is a reason the user can do something about, so it is the one reported.
		test::expect( lg::ocr::loadFailureActionable( "ONNX Runtime needs the Microsoft Visual C++ Redistributable" ) );
		test::expect( lg::ocr::loadFailureActionable( "cannot load onnxruntime.dll (Windows error 126)" ) );
	} );

	const test::Registrar ocr_missing_libraries( "ocr missing library report", [] {
		constexpr std::array names{ "one.dll", "two.dll", "three.dll" };
		// Answers that only the named libraries can be found.
		const auto found = []( std::vector<std::string> have ) {
			return [have = std::move( have )]( const char* name ) { return std::ranges::contains( have, std::string( name ) ); };
		};

		test::expectEqual( lg::ocr::missingLibraries( names, found( { "one.dll", "two.dll", "three.dll" } ) ), std::string() );
		test::expectEqual( lg::ocr::missingLibraries( names, found( { "two.dll" } ) ), std::string( "one.dll, three.dll" ) );
		test::expectEqual( lg::ocr::missingLibraries( names, found( {} ) ), std::string( "one.dll, two.dll, three.dll" ) );

#ifndef _WIN32
		// Nothing outside Windows imports it, so OCR is never held up there waiting for a runtime that does not apply.
		test::expectEqual( lg::ocr::missingVcRuntime(), std::string() );
#endif
	} );

} // namespace
