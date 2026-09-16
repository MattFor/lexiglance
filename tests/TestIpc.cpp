#include "IpcServer.h"
#include "Test.h"

#include <lexiglance/ipc/Socket.h>

#include <chrono>
#include <format>
#include <string>
#include <string_view>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#endif

namespace
{

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

} // namespace
