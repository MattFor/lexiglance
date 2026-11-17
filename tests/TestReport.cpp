#include "Test.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Report.h>
#include <lexiglance/core/Version.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

	namespace lg = lexiglance;

	void setEnvironment( const char* name, const std::string& value )
	{
#ifdef _WIN32
		( void )_putenv_s( name, value.c_str() );
#else
		( void )( value.empty() ? ::unsetenv( name ) : ::setenv( name, value.c_str(), 1 ) );
#endif
	}

	std::string environment( const char* name )
	{
		const char* value = std::getenv( name );
		return value != nullptr ? value : "";
	}

	// A report goes into a public issue: no AnkiConnect key, no password in an address, no secret parameter.
	const lg::test::Registrar report_redaction( "a problem report holds no secrets from the settings", [] {
		const std::string home_before = environment( "LEXIGLANCE_HOME" );
		const auto        home        = lg::test::scratch( "report-home" );
		setEnvironment( "LEXIGLANCE_HOME", home.string() );

		lg::config::Config config;
		config.anki.key = "s3cret-anki-key";
		config.anki.url = "http://someone:hunter2@127.0.0.1:8765";
		config.audio.sources.emplace_back( "https://tts.example/speak?term={term}&api_key=abc123xyz&lang=ja" );
		lg::test::expect( config.save( home / "config" / "config.json" ).has_value() );
		std::filesystem::create_directories( home / "state" );
		std::ofstream( home / "state" / "daemon.log" ) << "one\ntwo\nthree\n";

		const auto status  = lg::json::Document::parse( R"({"version":"9.9.9","backend":"x11","capture":"at-spi + OCR","capture_problem":"","languages":["ja"],"dictionaries":2})" );
		const auto health  = lg::json::Document::parse( R"({"checks":[{"id":"ocr","title":"OCR","status":"error","detail":"no model","fix":"open-scanning"},{"id":"x","title":"Fine","status":"ok","detail":"all good"}]})" );
		const auto listing = lg::json::Document::parse( R"({"dictionaries":[{"title":"JMdict","kind":"Words","source_language":"ja","target_language":"en","terms":5,"enabled":true}]})" );
		if ( !lg::test::expect( status.has_value() && health.has_value() && listing.has_value() ) )
		{
			return;
		}
		const lg::report::Input input{ .what = "The popup was empty", .program = "test", .status = &status->root(), .health = &health->root(), .dictionaries = &listing->root() };
		const auto              parts      = lg::report::parts( input );
		bool                    has_config = false;
		for ( const auto& part : parts )
		{
			lg::test::expect( !part.contents.contains( "s3cret-anki-key" ) && !part.contents.contains( "hunter2" ) && !part.contents.contains( "abc123xyz" ) );
			if ( part.name == "config.json" )
			{
				has_config = true;
				lg::test::expect( part.contents.contains( "<redacted>" ) && part.contents.contains( "{term}" ) && part.contents.contains( "lang=ja" ) );
			}
		}
		lg::test::expect( has_config );
		lg::test::expect( std::ranges::any_of( parts, []( const lg::report::Part& part ) { return part.name == "daemon.log" && part.contents == "one\ntwo\nthree\n"; } ) );
		lg::test::expect( std::ranges::any_of( parts, []( const lg::report::Part& part ) { return part.name == "dictionaries.txt" && part.contents.contains( "JMdict" ); } ) );

		const auto summary = lg::report::summary( input );
		lg::test::expect( summary.contains( "9.9.9" ) && summary.contains( "at-spi + OCR" ) && summary.contains( "error: OCR: no model" ) && !summary.contains( "all good" ) );

		lg::test::expectEqual( lg::report::withoutSecrets( "http://127.0.0.1:8765" ), std::string( "http://127.0.0.1:8765" ) );
		lg::test::expectEqual( lg::report::withoutSecrets( "https://a:b@host/x?token=t&q=1" ), std::string( "https://<redacted>@host/x?token=<redacted>&q=1" ) );
		// A password as typed, with characters an address keeps for itself.
		for ( const std::string password : { "pa/ss", "pa?ss", "pa#ss", "p@ss", "pa:ss/w?x#y@z" } )
		{
			const auto redacted = lg::report::withoutSecrets( "http://someone:" + password + "@127.0.0.1:8765/" );
			lg::test::expect( !redacted.contains( "someone" ) && !redacted.contains( "ss" ) && redacted.contains( "@127.0.0.1:8765/" ) );
		}
		lg::test::expectEqual( lg::report::withoutSecrets( "https://host/q?mail=a@b.c" ), std::string( "https://host/q?mail=a@b.c" ) );

		const auto zip = home / "out" / lg::report::fileName( std::chrono::system_clock::now() );
		lg::test::expect( lg::report::save( zip, parts ).has_value() && std::filesystem::file_size( zip ) > 100 );
		setEnvironment( "LEXIGLANCE_HOME", home_before );
	} );

	const lg::test::Registrar report_helpers( "a report names no home folder, keeps the end of a log and fits the issue link", [] {
#ifdef _WIN32
		const std::string home = environment( "USERPROFILE" );
#else
		const std::string home = environment( "HOME" );
#endif
		if ( home.size() > 1 )
		{
			lg::test::expectEqual( lg::report::withoutHome( home + "/x/y and " + home ), std::string( "~/x/y and ~" ) );
#ifdef _WIN32
			// Qt's spelling of the same folder, and the pipe named after the account.
			std::string forward = home;
			std::ranges::replace( forward, '\\', '/' );
			lg::test::expectEqual( lg::report::withoutHome( forward + "/AppData" ), std::string( "~/AppData" ) );
			lg::test::expectEqual( lg::report::withoutHome( "\\\\.\\pipe\\lexiglance-" + environment( "USERNAME" ) ), std::string( "\\\\.\\pipe\\lexiglance-~" ) );
			lg::test::expectEqual( lg::report::withoutHome( "cannot listen on lexiglance-gui-" + environment( "USERNAME" ) + "-1a2b" ), std::string( "cannot listen on lexiglance-gui-~-1a2b" ) );
#endif
		}

		const auto file = lg::test::scratch( "report-tail" ) / "log.txt";
		std::ofstream( file ) << "a\nb\nc\nd\n";
		lg::test::expect( lg::report::tail( file, 2 ) == std::vector<std::string>{ "c", "d" } );
		lg::test::expect( lg::report::tail( file.parent_path() / "missing", 5 ).empty() );

		const std::string long_text( 20000, 'x' );
		std::string       facts;
		for ( int i = 0; i < 400; ++i )
		{
			facts += "a line of the summary with spaces\n";
		}
		const auto url = lg::report::issueUrl( long_text, facts );
		lg::test::expect( url.size() <= 7000 && url.starts_with( std::string( lg::project_url ) + "/issues/new?template=bug_report.yml" ) );
		lg::test::expect( url.contains( "&what=xxx" ) && url.contains( "&version=" ) && url.contains( "&system=" ) && !url.contains( ' ' ) );
		const auto short_url = lg::report::issueUrl( "Popup: 空 & gone", "Health: 0 errors" );
		lg::test::expect( short_url.contains( "what=Popup%3A%20%E7%A9%BA%20%26%20gone" ) && short_url.contains( "summary=Health%3A%200%20errors" ) );
	} );

} // namespace
