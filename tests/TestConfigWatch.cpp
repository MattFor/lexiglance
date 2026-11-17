#include "Test.h"

#include "ConfigWatch.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{

	namespace test = lexiglance::test;
	using lexiglance::daemon::ConfigWatch;

	const test::Registrar config_watch( "the settings file changed by someone else is told from the daemon's own saves", [] {
		const auto  file = test::scratch( "config-watch" ) / "config.json";
		ConfigWatch watch( file );
		test::expect( !watch.changed() );

		std::ofstream( file ) << R"({"language":"ja"})";
		test::expect( watch.changed() );
		test::expect( !watch.changed() );

		std::ofstream( file ) << R"({"language":"ru","paused":true})";
		watch.own();
		test::expect( !watch.changed() );

		// Seen without being taken, as the daemon checks before it saves.
		std::ofstream( file ) << R"({"language":"ko"})";
		test::expect( watch.pending() && watch.pending() );
		test::expect( watch.changed() );
		test::expect( !watch.pending() );
		// As long as before, only later: an edit of one character.
		std::ofstream( file ) << R"({"language":"el"})";
		std::filesystem::last_write_time( file, std::filesystem::last_write_time( file ) + std::chrono::seconds( 2 ) );
		test::expect( watch.changed() );

		// Gone for a moment while an editor saves: the settings in use stay.
		std::filesystem::remove( file );
		test::expect( !watch.changed() );
	} );

} // namespace
