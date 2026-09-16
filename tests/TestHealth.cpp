#include "Test.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/ipc/Socket.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <unistd.h>
#endif

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	int currentPid()
	{
#ifdef _WIN32
		return static_cast<int>( GetCurrentProcessId() );
#else
		return static_cast<int>( ::getpid() );
#endif
	}

	const test::Registrar instance_lock( "one daemon at a time: the instance lock", [] {
		const auto                               file = test::scratch( "lock" ) / "daemon.lock";
		std::optional<lg::process::InstanceLock> first( std::in_place, file );
		test::expect( first->held() );
		// Another taker (another open file, as another process has) is refused and told who holds it.
		lg::process::InstanceLock second( file );
		test::expect( !second.held() );
		test::expectEqual( second.holder(), currentPid() );
		test::expect( !second.retry() );
		// Once the holder is gone, however it ended, the lock is free.
		first.reset();
		test::expect( second.retry() );
		test::expect( second.held() );
	} );

	const test::Registrar process_lookup( "processes of this user by name", [] {
#if defined( __linux__ ) || defined( _WIN32 )
		const int self = currentPid();
		// Linux keeps 15 characters of a process name ("lexiglance_test"); Windows names carry ".exe". Both match.
		test::expect( lg::process::isRunning( self, "lexiglance_tests" ) );
		test::expect( !lg::process::isRunning( self, "lexiglanced" ) );
		test::expect( !lg::process::isRunning( 0, "lexiglance_tests" ) );
		test::expect( lg::process::sameUser( self ) );
		const auto others = lg::process::othersNamed( "lexiglance_tests" );
		test::expect( std::ranges::find( others, self ) == others.end() );
		test::expectEqual( lg::process::executable().stem().string(), std::string( "lexiglance_tests" ) );
		test::expect( !lg::process::executableReplaced() );
#endif
	} );

	const test::Registrar health_report( "health report round trip", [] {
		const std::vector<lg::health::Check> checks{
			{ .id = "a", .title = "First", .status = lg::health::Severity::Ok, .detail = "fine" },
			{ .id = "b", .title = "Second", .status = lg::health::Severity::Error, .detail = "broken \"quoted\" ✗", .fix = "restart" },
			{ .id = "c", .title = "Third", .status = lg::health::Severity::Warning, .detail = "slow" },
			{ .id = "d", .title = "Fourth", .status = lg::health::Severity::Info, .detail = "note" },
		};
		lg::json::Writer out;
		lg::health::write( out, checks );
		const auto document = lg::json::Document::parse( out.take() );
		if ( !test::expect( document.has_value() ) )
		{
			return;
		}
		test::expectEqual( document->root()["errors"].asInt(), std::int64_t{ 1 } );
		test::expectEqual( document->root()["warnings"].asInt(), std::int64_t{ 1 } );
		const auto read = lg::health::read( document->root() );
		if ( !test::expect( read.size() == checks.size() ) )
		{
			return;
		}
		for ( std::size_t i = 0; i < read.size(); ++i )
		{
			test::expectEqual( read[i].id, checks[i].id );
			test::expectEqual( read[i].detail, checks[i].detail );
			test::expectEqual( read[i].fix, checks[i].fix );
			test::expect( read[i].status == checks[i].status );
		}
		test::expect( lg::health::parseStatus( "nonsense" ) == lg::health::Severity::Ok );
	} );

	const test::Registrar recent_problems( "warnings and errors are kept for the health report", [] {
		lg::log::warn( "test warning {}", 1 );
		lg::log::info( "not a problem" );
		const auto problems = lg::log::recentProblems();
		if ( !test::expect( !problems.empty() ) )
		{
			return;
		}
		test::expectEqual( problems.back().message, std::string( "test warning 1" ) );
		test::expect( problems.back().level == lg::log::Level::Warn );
		// Bounded, however much goes wrong.
		for ( int i = 0; i < 40; ++i )
		{
			lg::log::error( "test error {}", i );
		}
		test::expect( lg::log::recentProblems().size() <= 32 );
		test::expectEqual( lg::log::recentProblems().back().message, std::string( "test error 39" ) );
	} );

#ifdef _WIN32

	// A pipe server of the daemon's kind: one instance for one client.
	HANDLE pipeServer( const std::string& name )
	{
		const std::wstring wide( name.begin(), name.end() );
		return CreateNamedPipeW( wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr );
	}

	std::string pipeName( std::string_view purpose )
	{
		return std::format( "\\\\.\\pipe\\lexiglance-test-{}-{}", purpose, currentPid() );
	}

	const test::Registrar pipe_round_trip( "a request and its answer through a named pipe", [] {
		const auto   name   = pipeName( "answer" );
		const HANDLE server = pipeServer( name );
		if ( !test::expect( server != INVALID_HANDLE_VALUE ) )
		{
			return;
		}
		// The server reads one request line and answers it.
		std::thread daemon( [server] {
			if ( ConnectNamedPipe( server, nullptr ) == FALSE && GetLastError() != ERROR_PIPE_CONNECTED )
			{
				return;
			}
			std::string request;
			char        c    = 0;
			DWORD       read = 0;
			while ( ReadFile( server, &c, 1, &read, nullptr ) != FALSE && read == 1 && c != '\n' )
			{
				request.push_back( c );
			}
			const std::string answer  = request.contains( "\"status\"" ) ? R"({"id":1,"result":{"version":"test"}})"
			                                                               "\n"
			                                                             : R"({"id":1,"error":{"message":"unknown"}})"
			                                                               "\n";
			DWORD             written = 0;
			WriteFile( server, answer.data(), static_cast<DWORD>( answer.size() ), &written, nullptr );
			FlushFileBuffers( server );
		} );
		auto        client = lg::ipc::Client::connect( name, std::chrono::seconds( 5 ) );
		if ( test::expect( client.has_value() ) )
		{
			const auto reply = client->call( "status" );
			test::expect( reply.has_value() && reply->root()["result"]["version"].asString() == "test" );
		}
		daemon.join();
		CloseHandle( server );
	} );

	const test::Registrar client_timeout( "a daemon that does not answer cannot block its clients", [] {
		const auto   name   = pipeName( "silent" );
		const HANDLE server = pipeServer( name );
		if ( !test::expect( server != INVALID_HANDLE_VALUE ) )
		{
			return;
		}
		// Connected, but the server never reads or answers.
		const auto started = std::chrono::steady_clock::now();
		auto       client  = lg::ipc::Client::connect( name, std::chrono::milliseconds( 200 ) );
		if ( test::expect( client.has_value() ) )
		{
			const auto reply = client->call( "status" );
			test::expect( !reply.has_value() && reply.error().message.contains( "timed out" ) );
			test::expect( std::chrono::steady_clock::now() - started < std::chrono::seconds( 3 ) );
		}
		// No server at all: refused at once.
		test::expect( !lg::ipc::connectTo( pipeName( "nobody" ), std::chrono::milliseconds( 200 ) ).has_value() );
		CloseHandle( server );
	} );

#else

	const test::Registrar client_timeout( "a daemon that does not answer cannot block its clients", [] {
		// Socket paths are short; the scratch directory may be too deep for one.
		const auto endpoint = ( std::filesystem::temp_directory_path() / std::format( "lg-test-{}.sock", currentPid() ) ).string();
		// A listener whose kernel backlog accepts the connection, but which never reads or answers.
		auto server = lg::ipc::listenOn( endpoint );
		if ( !test::expect( server.has_value() ) )
		{
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		auto       client  = lg::ipc::Client::connect( endpoint, std::chrono::milliseconds( 200 ) );
		if ( test::expect( client.has_value() ) )
		{
			const auto reply = client->call( "status" );
			test::expect( !reply.has_value() && reply.error().message.contains( "timed out" ) );
			test::expect( std::chrono::steady_clock::now() - started < std::chrono::seconds( 2 ) );
		}
		// A listener that is alive but stuck still counts as another instance.
		test::expect( !lg::ipc::listenOn( endpoint ).has_value() );
		server->close();
		std::error_code ec;
		std::filesystem::remove( endpoint, ec );
	} );

#endif

	const test::Registrar config_popup_look( "configuration round trip of the popup's design, colours and highlight", [] {
		lg::config::Config config;
		// New configurations start friendly.
		test::expect( config.popup.design == lg::config::PopupDesign::Friendly );

		auto& popup               = config.popup;
		popup.design              = lg::config::PopupDesign::Compact;
		popup.scheme              = lg::config::ColorScheme::Sakura;
		popup.placement           = lg::config::PopupPlacement::AboveText;
		popup.background_color    = "#101820";
		popup.accent_color        = "#ff8800";
		popup.colors              = { { .name = "muted", .value = "#777777" } };
		popup.corner_radius       = 4;
		popup.border_width        = 0;
		popup.padding             = 20;
		popup.opacity             = 85;
		popup.headword_size       = 40;
		popup.furigana_size       = 20;
		popup.show_reading        = false;
		popup.show_inflection     = false;
		popup.show_dictionary     = false;
		popup.show_buttons        = false;
		popup.show_kanji          = false;
		popup.max_senses          = 3;
		popup.highlight_style     = lg::config::HighlightStyle::WavyUnderline;
		popup.highlight_radius    = 6;
		popup.highlight_padding_x = 5;
		popup.highlight_padding_y = -3;
		popup.button_size         = 28;
		const auto parsed         = lg::config::Config::parse( config.toJson() );
		if ( !test::expect( parsed.has_value() ) )
		{
			return;
		}
		const auto& p = parsed->popup;
		test::expect( p.design == lg::config::PopupDesign::Compact );
		test::expect( p.scheme == lg::config::ColorScheme::Sakura );
		test::expect( p.placement == lg::config::PopupPlacement::AboveText );
		test::expectEqual( p.background_color, std::string( "#101820" ) );
		test::expectEqual( p.accent_color, std::string( "#ff8800" ) );
		test::expect( p.text_color.empty() && p.border_color.empty() );
		test::expect( p.colors.size() == 1 && p.colors.front().name == "muted" && p.colors.front().value == "#777777" );
		test::expectEqual( p.corner_radius, 4 );
		test::expectEqual( p.border_width, 0 );
		test::expectEqual( p.padding, 20 );
		test::expectEqual( p.opacity, 85 );
		test::expectEqual( p.headword_size, 40 );
		test::expectEqual( p.furigana_size, 20 );
		test::expect( !p.show_reading && !p.show_inflection && !p.show_dictionary && !p.show_buttons && !p.show_kanji );
		test::expectEqual( p.max_senses, 3 );
		test::expect( p.highlight_style == lg::config::HighlightStyle::WavyUnderline );
		test::expectEqual( p.highlight_radius, 6 );
		test::expectEqual( p.highlight_padding_x, 5 );
		test::expectEqual( p.highlight_padding_y, -3 );
		test::expectEqual( p.button_size, 28 );
		// The single room of older settings files becomes both.
		const auto older = lg::config::Config::parse( R"({"popup":{"highlight_padding":4}})" );
		test::expect( older && older->popup.highlight_padding_x == 4 && older->popup.highlight_padding_y == 4 );

		// Out of range values are clamped; malformed colours and unknown names fall back to the defaults.
		const auto odd = lg::config::Config::parse(
				R"({"popup":{"design":"fancy","scheme":"rainbow","placement":"sideways","opacity":5,"corner_radius":99,"background_color":"red",)"
				R"("accent_color":"#12345","colors":{"muted":"#abcdef","text":"blue"},"highlight_style":"zigzag","max_senses":-4}})"
		);
		if ( !test::expect( odd.has_value() ) )
		{
			return;
		}
		const auto& o = odd->popup;
		test::expect( o.design == lg::config::PopupDesign::Friendly );
		test::expect( o.scheme == lg::config::ColorScheme::Default );
		test::expect( o.placement == lg::config::PopupPlacement::BelowText );
		test::expectEqual( o.opacity, 30 );
		test::expectEqual( o.corner_radius, 32 );
		test::expect( o.background_color.empty() && o.accent_color.empty() );
		test::expect( o.colors.size() == 1 && o.colors.front().name == "muted" );
		test::expect( o.highlight_style == lg::config::HighlightStyle::Underline );
		test::expectEqual( o.max_senses, 0 );
	} );

} // namespace
