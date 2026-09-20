#include "Test.h"

#include "Stats.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>

#include <chrono>
#include <fstream>

namespace
{

	namespace test = lexiglance::test;
	namespace json = lexiglance::json;
	using lexiglance::daemon::Statistics;

	// Today's entry of the tally's `days`, as the Statistics page gets it.
	json::Value today( const json::Document& stats )
	{
		const auto name = lexiglance::log::day( std::chrono::system_clock::now() );
		for ( const json::Value& day : stats.root()["days"].items() )
		{
			if ( day["day"].asString() == name )
			{
				return day;
			}
		}
		return {};
	}

	const test::Registrar daily_figures( "statistics keep every figure of each day, translations included", [] {
		Statistics stats;
		stats.load( test::scratch( "statistics-days" ) / "statistics.json" );
		stats.recordLookup( "図書館", "ja", "ocr", 3, std::chrono::microseconds( 40 ) );
		stats.recordLookup( "", "", "ocr", 5, std::chrono::microseconds( 20 ) );
		stats.recordPopup();
		stats.recordAudio();
		stats.recordTranslation( "ja", 23, std::chrono::milliseconds( 180 ) );
		stats.recordTranslation( "ja", 23, std::chrono::microseconds::zero() );
		stats.recordTranslation( "ru", 10, std::chrono::milliseconds( 220 ) );

		const auto document = json::Document::parse( stats.json() );
		if ( !test::expect( document.has_value() ) )
		{
			return;
		}
		const json::Value& root = document->root();
		test::expectEqual( root["lookups"].asInt(), std::int64_t{ 2 } );
		test::expectEqual( root["translations"].asInt(), std::int64_t{ 3 } );
		test::expectEqual( root["translated_characters"].asInt(), std::int64_t{ 56 } );
		// Remembered translations are not timed: (180 + 220) / 2.
		test::expect( root["average_translation_ms"].asDouble() > 199.0 && root["average_translation_ms"].asDouble() < 201.0 );
		test::expectEqual( root["translated_languages"].items().front()["language"].asString(), std::string_view( "ja" ) );

		const json::Value day = today( *document );
		test::expectEqual( day["lookups"].asInt(), std::int64_t{ 2 } );
		test::expectEqual( day["found"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( day["characters"].asInt(), std::int64_t{ 8 } );
		test::expectEqual( day["popups"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( day["audio"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( day["translations"].asInt(), std::int64_t{ 3 } );
		test::expectEqual( day["translated_characters"].asInt(), std::int64_t{ 56 } );
	} );

	const test::Registrar counted_apart( "statistics count lookups and translations each only while that is ticked", [] {
		Statistics stats;
		stats.load( test::scratch( "statistics-apart" ) / "statistics.json" );
		stats.setEnabled( true, false );
		stats.recordLookup( "本", "ja", "ocr", 1, std::chrono::microseconds( 10 ) );
		stats.recordTranslation( "ja", 12, std::chrono::milliseconds( 100 ) );
		stats.setEnabled( false, true );
		stats.recordLookup( "本", "ja", "ocr", 1, std::chrono::microseconds( 10 ) );
		stats.recordPopup();
		stats.recordTranslation( "ru", 8, std::chrono::milliseconds( 100 ) );

		const auto document = json::Document::parse( stats.json() );
		if ( !test::expect( document.has_value() ) )
		{
			return;
		}
		const json::Value& root = document->root();
		test::expectEqual( root["lookups"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( root["popups"].asInt(), std::int64_t{ 0 } );
		test::expectEqual( root["translations"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( root["translated_languages"].items().front()["language"].asString(), std::string_view( "ru" ) );
		test::expect( !root["enabled"].asBool() );
		test::expect( root["translations_enabled"].asBool() );
	} );

	const test::Registrar saved_and_read( "statistics are written and read back, a tally from before 1.3.0 too", [] {
		const auto file = test::scratch( "statistics-file" ) / "statistics.json";
		{
			Statistics stats;
			stats.load( file );
			stats.recordLookup( "книга", "ru", "at-spi", 5, std::chrono::microseconds( 30 ) );
			stats.recordTranslation( "ru", 12, std::chrono::milliseconds( 150 ) );
			stats.save();
		}
		Statistics again;
		again.load( file );
		const auto document = json::Document::parse( again.json() );
		if ( !test::expect( document.has_value() ) )
		{
			return;
		}
		test::expectEqual( document->root()["translations"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( today( *document )["translations"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( document->root()["sessions"].asInt(), std::int64_t{ 2 } );

		// Before 1.3.0 a day was only its number of lookups.
		const auto old_file = test::scratch( "statistics-old" ) / "statistics.json";
		std::ofstream( old_file ) << R"({"lookups":7,"found":5,"days":{"2026-09-01":4,"2026-09-02":3}})";
		Statistics old;
		old.load( old_file );
		const auto converted = json::Document::parse( old.json() );
		if ( test::expect( converted.has_value() ) )
		{
			const auto& days = converted->root()["days"].items();
			test::expect( days.size() >= 2 );
			test::expectEqual( days.front()["day"].asString(), std::string_view( "2026-09-01" ) );
			test::expectEqual( days.front()["lookups"].asInt(), std::int64_t{ 4 } );
			test::expectEqual( days.front()["translations"].asInt(), std::int64_t{ 0 } );
		}
	} );

} // namespace
