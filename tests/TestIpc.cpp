#include "IpcServer.h"
#include "Test.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Uninstall.h>
#include <lexiglance/ipc/Socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#endif

namespace
{

	namespace fs = std::filesystem;
	namespace lg = lexiglance;
	using namespace std::chrono_literals;

	std::string endpoint()
	{
#ifdef _WIN32
		return std::format( "\\\\.\\pipe\\lexiglance-test-{}", GetCurrentProcessId() );
#else
		return ( lg::test::scratch( "ipc-server" ) / "daemon.sock" ).string();
#endif
	}

	void setHome( const std::string& value )
	{
#ifdef _WIN32
		( void )_putenv_s( "LEXIGLANCE_HOME", value.c_str() );
#else
		( void )( value.empty() ? ::unsetenv( "LEXIGLANCE_HOME" ) : ::setenv( "LEXIGLANCE_HOME", value.c_str(), 1 ) );
#endif
	}

	// LEXIGLANCE_HOME relocates the daemon's endpoint too, so a scratch home's daemon runs beside the user's own.
	const lg::test::Registrar ipc_endpoint_home( "a home of its own (LEXIGLANCE_HOME) has a daemon endpoint of its own", [] {
		const char*       saved  = std::getenv( "LEXIGLANCE_HOME" );
		const std::string before = saved != nullptr ? saved : "";
		setHome( "" );
		const std::string plain = lg::paths::ipcEndpoint();
		setHome( lg::test::scratch( "endpoint-a" ).string() );
		const std::string a = lg::paths::ipcEndpoint();
		setHome( lg::test::scratch( "endpoint-b" ).string() );
		const std::string b = lg::paths::ipcEndpoint();
		setHome( before );
		lg::test::expect( plain != a && plain != b && a != b );
	} );

	// The daemon's server loop: a Unix socket with poll(), or on Windows named pipe instances with overlapped reads.
	const lg::test::Registrar ipc_server( "the daemon's server answers several clients and sends them events", [] {
		lg::daemon::IpcServer server;
		server.on( "echo", []( const lg::json::Value& params ) -> lg::Result<std::string> {
			lg::json::Writer out;
			out.beginObject().field( "text", params["text"].asString() ).endObject();
			return out.take();
		} );
		server.on( "fail", []( const lg::json::Value& ) -> lg::Result<std::string> { return lg::fail( "on purpose" ); } );
		const auto name = endpoint();
		if ( !lg::test::expect( server.start( name ).has_value() ) )
		{
			return;
		}
		// Another server cannot take the endpoint over.
		lg::test::expect( !lg::ipc::listenOn( name ).has_value() );

		auto first  = lg::ipc::Client::connect( name, 2s );
		auto second = lg::ipc::Client::connect( name, 2s );
		if ( !lg::test::expect( first.has_value() && second.has_value() ) )
		{
			return;
		}
		for ( int round = 0; round < 3; ++round )
		{
			for ( auto* client : { &*first, &*second } )
			{
				const auto reply = client->call( "echo", R"({"text":"日本語"})" );
				lg::test::expect( reply.has_value() && reply->root()["result"]["text"].asString() == "日本語" );
			}
		}
		const auto failed = first->call( "fail" );
		lg::test::expect( !failed.has_value() && failed.error().message == "on purpose" );
		lg::test::expect( !second->call( "no.such.method" ).has_value() );

		// An event sent before a request reaches the client before the answer.
		server.broadcast( "tick", R"({"n":1})" );
		bool       ticked = false;
		const auto reply  = first->call( "echo", R"({"text":"a"})", [&]( std::string_view event, const lg::json::Value& params ) {
			ticked = ticked || ( event == "tick" && params["n"].asInt() == 1 );
		} );
		lg::test::expect( reply.has_value() && ticked );

		// A client leaving does not disturb the others.
		{
			auto third = lg::ipc::Client::connect( name, 2s );
			lg::test::expect( third.has_value() && third->call( "echo", R"({"text":"b"})" ).has_value() );
		}
		lg::test::expect( second->call( "echo", R"({"text":"c"})" ).has_value() );

		server.stop();
		lg::test::expect( !lg::ipc::connectTo( name, 300ms ).has_value() );
	} );

	// Handlers that wait (health, translate, Anki, audio) run on workers: another client is answered meanwhile, and a
	// client's own requests are still answered in the order it sent them.
	const lg::test::Registrar ipc_slow_request( "a slow request holds up no other client, and its own answers keep their order", [] {
		std::promise<void>    release;
		const auto            released = release.get_future().share();
		std::atomic<int>      started{ 0 };
		lg::daemon::IpcServer server;
		server.onSlow( "slow", [&]( const lg::json::Value& ) -> lg::Result<std::string> {
			++started;
			released.wait_for( 10s );
			return std::string( R"({"slow":true})" );
		} );
		server.on( "ping", []( const lg::json::Value& ) -> lg::Result<std::string> { return std::string( "{}" ); } );
		const auto name = endpoint();
		if ( !lg::test::expect( server.start( name ).has_value() ) )
		{
			return;
		}
		auto first  = lg::ipc::Client::connect( name, 2s );
		auto second = lg::ipc::Client::connect( name, 2s );
		auto third  = lg::ipc::connectTo( name, 2s );
		if ( !lg::test::expect( first.has_value() && second.has_value() && third.has_value() ) )
		{
			return;
		}

		// A bool, not the reply: GCC 14's LTO misreads a Result<Document> passed through std::async (-Wfree-nonheap-object).
		auto slow = std::async( std::launch::async, [&] {
			const auto reply = first->call( "slow" );
			return reply.has_value() && reply->root()["result"]["slow"].asBool();
		} );
		// The third client sends a slow request and a quick one behind it at once.
		lg::test::expect( third->writeAll( lg::ipc::request( 1, "slow" ) + lg::ipc::request( 2, "ping" ) ).has_value() );
		for ( int waited = 0; started < 2 && waited < 500; ++waited )
		{
			std::this_thread::sleep_for( 10ms );
		}
		lg::test::expect( started == 2 );
		lg::test::expect( second->call( "ping" ).has_value() );
		lg::test::expect( slow.wait_for( 0s ) != std::future_status::ready );

		release.set_value();
		lg::test::expect( slow.get() );
		lg::ipc::LineBuffer       buffer;
		std::vector<std::int64_t> ids;
		std::array<char, 4096>    chunk{};
		while ( ids.size() < 2 )
		{
			const auto received = third->readSome( chunk );
			if ( !received || *received == 0 )
			{
				break;
			}
			buffer.append( std::string_view( chunk.data(), *received ) );
			while ( auto line = buffer.next() )
			{
				if ( auto message = lg::json::Document::parse( std::move( *line ) ) )
				{
					ids.push_back( message->root()["id"].asInt( -1 ) );
				}
			}
		}
		lg::test::expect( ids == std::vector<std::int64_t>{ 1, 2 } );
		server.stop();
	} );

	const lg::test::Registrar ipc_stuck_client( "the daemon's server hangs up on a client that stops reading", [] {
		lg::daemon::IpcServer server;
		server.on( "ping", []( const lg::json::Value& ) -> lg::Result<std::string> { return std::string( "{}" ); } );
		const auto name = endpoint();
		if ( !lg::test::expect( server.start( name ).has_value() ) )
		{
			return;
		}
		auto stuck = lg::ipc::Client::connect( name, 10s );
		if ( !lg::test::expect( stuck.has_value() && stuck->call( "ping" ).has_value() ) )
		{
			return;
		}
		// Far more than a socket or pipe buffer holds, and the stuck client never reads it.
		const auto params = std::format( R"({{"text":"{}"}})", std::string( 4096, 'x' ) );
		for ( int i = 0; i < 1024; ++i )
		{
			server.broadcast( "fill", params );
		}
		// Served once the write to the stuck client has timed out.
		auto other = lg::ipc::Client::connect( name, 15s );
		lg::test::expect( other.has_value() && other->call( "ping" ).has_value() );
		lg::test::expect( !stuck->call( "ping" ).has_value() );
		server.stop();
	} );

	// What uninstalling a home of its own (LEXIGLANCE_HOME) stops: its daemon and settings window, asked over their
	// sockets, and no process by name (the user's own Lexiglance runs beside the tests).
	const lg::test::Registrar stop_home( "uninstall of a home of its own asks only its own daemon and window to quit", [] {
		const char*       saved  = std::getenv( "LEXIGLANCE_HOME" );
		const std::string before = saved != nullptr ? saved : "";
		setHome( lg::test::scratch( "uninstall-stop-home" ).string() );
		const fs::path runtime = lg::paths::runtimeDir();
		( void )lg::paths::ensureDirectory( fs::path( lg::paths::ipcEndpoint() ).parent_path(), true );
		( void )lg::paths::ensureDirectory( runtime, true );

		// This home's daemon: it holds the lock, and lets go of it when asked to shut down.
		auto                  lock = std::make_unique<lg::process::InstanceLock>( runtime / "daemon.lock" );
		std::atomic<bool>     asked{ false };
		lg::daemon::IpcServer server;
		server.on( "shutdown", [&]( const lg::json::Value& ) -> lg::Result<std::string> {
			asked = true;
			lock.reset();
			return std::string( "{}" );
		} );
		const bool running = lock->held() && server.start( lg::paths::ipcEndpoint() ).has_value();
		lg::test::expect( running );

#ifndef _WIN32
		// Its settings window: reads one request, and is gone after "quit".
		const std::string window    = ( fs::path( lg::paths::ipcEndpoint() ).parent_path() / "gui.sock" ).string();
		auto              listening = lg::ipc::listenOn( window );
		std::string       request;
		std::jthread      serving;
		if ( lg::test::expect( listening.has_value() ) )
		{
			serving = std::jthread( [&] {
				if ( auto client = lg::ipc::acceptFrom( *listening ) )
				{
					std::array<char, 64> chunk{};
					while ( !request.contains( '\n' ) )
					{
						const auto received = client->readSome( chunk );
						if ( !received || *received == 0 )
						{
							break;
						}
						request.append( chunk.data(), *received );
					}
				}
				listening->close();
				fs::remove( window );
			} );
		}
#endif

		const auto started = std::chrono::steady_clock::now();
		if ( running )
		{
			lg::uninstall::stopHome( lg::uninstall::Places::current(), 3s );
		}
		lg::test::expect( asked );
		lg::test::expect( std::chrono::steady_clock::now() - started < 2s );
		lg::test::expect( lg::process::InstanceLock( runtime / "daemon.lock" ).held() );
#ifndef _WIN32
		// A window never asked would wait for good.
		if ( serving.joinable() )
		{
			( void )lg::ipc::connectTo( window, 300ms );
			serving.join();
		}
		lg::test::expectEqual( request, std::string( "quit\n" ) );
#endif
		server.stop();
		setHome( before );
	} );

} // namespace
