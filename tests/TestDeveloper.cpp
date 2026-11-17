#include "Developer.h"
#include "IpcServer.h"
#include "Test.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/ipc/Socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <format>
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

	namespace lg = lexiglance;
	using namespace std::chrono_literals;

	std::string endpoint( std::string_view which )
	{
#ifdef _WIN32
		return std::format( "\\\\.\\pipe\\lexiglance-test-{}-{}", which, GetCurrentProcessId() );
#else
		return ( lg::test::scratch( which ) / "daemon.sock" ).string();
#endif
	}

	// Each stage of a trace event carries its median and 95th percentile over the latest lookups.
	const lg::test::Registrar trace_event( "a trace event times its stages, with their median and 95th percentile", [] {
		lg::daemon::Tracer tracer;
		lg::test::expect( !tracer.on() );
		std::string last;
		for ( int ms = 1; ms <= 100; ++ms )
		{
			const std::array<lg::daemon::Tracer::Timing, 2> timings{ { { .stage = lg::daemon::Stage::Capture, .took = std::chrono::milliseconds( ms ) }, { .stage = lg::daemon::Stage::Lookup, .took = 250us } } };
			last = tracer.event( 7, timings, []( lg::json::Writer& out ) { out.field( "source", "ocr" ); } );
		}
		const auto parsed = lg::json::Document::parse( last );
		if ( !lg::test::expect( parsed.has_value() ) )
		{
			return;
		}
		const auto& root    = parsed->root();
		const auto& capture = root["stages"]["capture"];
		lg::test::expectEqual( root["id"].asInt(), 7 );
		lg::test::expectEqual( root["source"].asString(), std::string_view( "ocr" ) );
		lg::test::expectEqual( capture["ms"].asDouble(), 100.0 );
		lg::test::expectEqual( capture["p50"].asDouble(), 50.0 );
		lg::test::expectEqual( capture["p95"].asDouble(), 95.0 );
		lg::test::expectEqual( root["stages"]["lookup"]["p95"].asDouble(), 0.25 );
		lg::test::expect( root["stages"]["render"].isNull() && root["time"].asString().size() == 12 );
	} );

	// Topics reach only the clients that subscribed, and nobody listening means nothing is made for them.
	const lg::test::Registrar ipc_topics( "subscribed events reach only their subscribers, and stop with them", [] {
		lg::daemon::IpcServer server;
		server.on( "echo", []( const lg::json::Value& ) -> lg::Result<std::string> { return std::string( "{}" ); } );
		const auto name = endpoint( "ipc-topics" );
		if ( !lg::test::expect( server.start( name ).has_value() ) )
		{
			return;
		}
		const lg::daemon::Tracer tracer( &server );
		auto                     others = lg::ipc::Client::connect( name, 2s );
		lg::test::expect( !tracer.on() && !server.listening( lg::daemon::IpcServer::Topic::Log ) );
		{
			auto listener = lg::ipc::Client::connect( name, 2s );
			if ( !lg::test::expect( listener.has_value() && others.has_value() ) )
			{
				return;
			}
			lg::test::expect( !listener->call( "events.subscribe", R"({"events":["nonsense"]})" ).has_value() );
			lg::test::expect( listener->call( "events.subscribe", R"({"events":["trace"]})" ).has_value() );
			lg::test::expect( tracer.on() && !server.listening( lg::daemon::IpcServer::Topic::Log ) );

			server.publish( lg::daemon::IpcServer::Topic::Trace, "trace", R"({"id":1})" );
			server.broadcast( "tick", "{}" );
			int        traces = 0;
			const auto count  = [&]( std::string_view event, const lg::json::Value& ) { traces += event == "trace" ? 1 : 0; };
			lg::test::expect( listener->call( "echo", "{}", count ).has_value() );
			lg::test::expectEqual( traces, 1 );
			bool       ticked    = false;
			bool       overheard = false;
			const auto watch     = [&]( std::string_view event, const lg::json::Value& ) {
				ticked    = ticked || event == "tick";
				overheard = overheard || event == "trace";
			};
			lg::test::expect( others->call( "echo", "{}", watch ).has_value() );
			lg::test::expect( ticked && !overheard );

			// Subscribing again replaces what was asked for.
			lg::test::expect( listener->call( "events.subscribe", R"({"events":["log"]})" ).has_value() );
			lg::test::expect( !tracer.on() && server.listening( lg::daemon::IpcServer::Topic::Log ) );
		}
		// The subscriber hung up: nobody listens any more.
		bool gone = false;
		for ( int i = 0; i < 100 && !gone; ++i )
		{
			gone = !server.listening( lg::daemon::IpcServer::Topic::Log );
			if ( !gone )
			{
				( void )others->call( "echo" );
				std::this_thread::sleep_for( 20ms );
			}
		}
		lg::test::expect( gone );
		server.stop();
	} );

	// Developer mode's topics: refused while it is off, and stopped when it goes off.
	const lg::test::Registrar ipc_topic_guard( "subscribing needs what the guard asks, and subscriptions end once it no longer holds", [] {
		std::atomic<bool>     allowed{ false };
		lg::daemon::IpcServer server;
		server.on( "echo", []( const lg::json::Value& ) -> lg::Result<std::string> { return std::string( "{}" ); } );
		server.guardTopics( [&]() -> lg::Result<> { return allowed ? lg::Result<>{} : lg::fail( "developer mode is off" ); } );
		const auto name = endpoint( "ipc-guard" );
		if ( !lg::test::expect( server.start( name ).has_value() ) )
		{
			return;
		}
		auto client = lg::ipc::Client::connect( name, 2s );
		if ( !lg::test::expect( client.has_value() ) )
		{
			return;
		}
		const auto refused = client->call( "events.subscribe", R"({"events":["trace"]})" );
		lg::test::expect( !refused.has_value() && refused.error().message == "developer mode is off" );
		lg::test::expect( client->call( "events.subscribe", R"({"events":[]})" ).has_value() );
		allowed = true;
		lg::test::expect( client->call( "events.subscribe", R"({"events":["trace"]})" ).has_value() && server.listening( lg::daemon::IpcServer::Topic::Trace ) );
		allowed = false;
		server.publish( lg::daemon::IpcServer::Topic::Trace, "trace", R"({"id":1})" );
		int        traces = 0;
		const auto count  = [&]( std::string_view event, const lg::json::Value& ) { traces += event == "trace" ? 1 : 0; };
		lg::test::expect( client->call( "echo", "{}", count ).has_value() );
		lg::test::expectEqual( traces, 0 );
		lg::test::expect( !server.listening( lg::daemon::IpcServer::Topic::Trace ) );
		server.stop();
	} );

	const lg::test::Registrar simulated_problems( "simulated problems come with their severity and fix, and take the place of real ones", [] {
		for ( const std::string_view name : lg::daemon::simulated_problems )
		{
			lg::test::expect( !lg::daemon::simulatedProblem( name ).empty() );
		}
		lg::test::expect( lg::daemon::simulatedProblem( "nonsense" ).empty() );
		lg::test::expect( std::ranges::all_of( lg::daemon::simulatedProblem( "permissions" ), &lg::health::Check::simulated ) );
		// Told apart from real ones on the other end of the socket (the settings application must not act on them).
		lg::json::Writer written;
		lg::health::write( written, lg::daemon::simulatedProblem( "ocr" ) );
		const auto report = lg::json::Document::parse( written.str() );
		lg::test::expect( report.has_value() && lg::health::read( report->root() ).front().simulated );
		lg::test::expect( lg::daemon::simulatedProblem( "error" ).front().status == lg::health::Severity::Error && !lg::daemon::simulatedProblem( "error" ).front().fix.empty() );
		const auto permissions = lg::daemon::simulatedProblem( "permissions" );
		lg::test::expect( std::ranges::all_of( permissions, []( const lg::health::Check& check ) { return check.fix.starts_with( "open-privacy-" ); } ) );

		std::vector<lg::health::Check> checks{ { .id = "ocr", .title = "OCR", .status = lg::health::Severity::Ok, .detail = "fine" }, { .id = "daemon", .title = "Daemon" } };
		lg::daemon::addSimulated( checks, lg::daemon::simulatedProblem( "ocr" ) );
		lg::daemon::addSimulated( checks, lg::daemon::simulatedProblem( "info" ) );
		lg::test::expectEqual( checks.size(), std::size_t( 3 ) );
		lg::test::expect( checks[0].status == lg::health::Severity::Error && checks[1].id == "daemon" && checks[2].id == "simulated-info" );
	} );

	const lg::test::Registrar inspection_picture( "the capture inspector draws what OCR read as a PNG", [] {
		lg::platform::Snapshot                  shot{ .region = { .x = 100, .y = 50, .width = 40, .height = 20 }, .rgb = std::vector<std::uint8_t>( 40UL * 20UL * 3UL, 255 ), .lines = { { .box = { .x = 2, .y = 2, .width = 30, .height = 12 }, .text = "日本", .loose = false } } };
		const std::array<lg::platform::Rect, 1> text{ { { .x = 102, .y = 52, .width = 10, .height = 12 } } };
		const auto                              png = lg::daemon::inspectionPng( shot, { .x = 105, .y = 58 }, text, { .x = 102, .y = 52, .width = 5, .height = 12 } );
		lg::test::expect( png.starts_with( "\x89PNG" ) );
		shot.rgb.pop_back();
		lg::test::expect( lg::daemon::inspectionPng( shot, { .x = 105, .y = 58 }, text, {} ).empty() );
		lg::test::expectEqual( lg::daemon::base64( "Man" ), std::string( "TWFu" ) );
		lg::test::expectEqual( lg::daemon::base64( "Ma" ), std::string( "TWE=" ) );
	} );

} // namespace
