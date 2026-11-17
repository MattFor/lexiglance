// Developer mode's side of the daemon (docs/debugging.md): the capture inspector, scans and lookups simulated through
// the pipeline (never input injected into the system), made-up problems, the trace and the log as they happen.

#include "Daemon.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Report.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iterator>
#include <memory>

namespace lexiglance::daemon
{

	namespace
	{

		void writeRect( json::Writer& out, const platform::Rect& rect )
		{
			out.beginObject().field( "x", rect.x ).field( "y", rect.y ).field( "width", rect.width ).field( "height", rect.height ).endObject();
		}

		platform::Point pointOf( const json::Value& params )
		{
			return { .x = static_cast<int>( params["x"].asInt() ), .y = static_cast<int>( params["y"].asInt() ) };
		}

		std::string_view kindOf( bool text, int length, bool sentence )
		{
			if ( text )
			{
				return "text";
			}
			if ( length > 0 )
			{
				return "wheel";
			}
			return sentence ? "sentence" : "scan";
		}

	} // namespace

	Daemon::Probe Daemon::inspect( CaptureState& state, const CaptureRequest& request, std::optional<platform::CapturedText> captured, std::chrono::microseconds took )
	{
		Probe probe{ .captured = std::move( captured ), .lines = {}, .snapshot = std::nullopt, .took = took, .window = request.window };
		if ( probe.captured && state.capture )
		{
			probe.lines = state.capture->lineBounds( *probe.captured, utf8::length( probe.captured->text ) );
		}
		// The capture that read the text knows what it read it from; with nothing read, OCR may still have seen lines.
		platform::TextCapture* origin = probe.captured && probe.captured->origin != nullptr ? probe.captured->origin : state.capture.get();
		if ( origin != nullptr )
		{
			probe.snapshot = origin->snapshot( request.point, request.window );
		}
		return probe;
	}

	void Daemon::traceLookup( const CaptureRequest& request, const platform::CapturedText& captured, const lookup::LookupResult& result, std::string_view source, std::string_view unread, std::chrono::microseconds took )
	{
		if ( !tracer_.on() )
		{
			return;
		}
		tracer_.send(
				request.generation,
				{ { .stage = Stage::Capture, .took = took }, { .stage = Stage::Lookup, .took = result.elapsed } },
				[&]( json::Writer& out ) {
					out.field( "kind", kindOf( request.text.has_value(), request.length, request.sentence ) )
							.field( "x", request.point.x )
							.field( "y", request.point.y )
							.field( "window", request.window.wm_class )
							.field( "source", source )
							.field( "text", utf8::prefix( captured.text, 120 ) )
							.field( "unread", utf8::prefix( unread, 120 ) )
							.field( "offset", captured.sentence_offset )
							.field( "rewound", captured.rewound )
							.field( "confidence", static_cast<double>( captured.confidence ) )
							.field( "characters", utf8::length( captured.text ) )
							.field( "found", result.terms.size() + result.kanji.size() )
							.field( "matched", result.matched_length )
							.field( "language", result.language != nullptr ? result.language->code() : std::string_view() );
				}
		);
	}

	void Daemon::writeSimulated( json::Writer& out, std::string_view real ) const
	{
		const std::scoped_lock lock( problems_mutex_ );
		// A simulated OCR or permission problem is what the Overview and the Scanning page show the capture lacking.
		const auto capture = std::ranges::find_if( simulated_, []( const health::Check& check ) { return check.id == "ocr" || check.id == "accessibility"; } );
		out.field( "capture_problem", capture != simulated_.end() ? std::string_view( capture->detail ) : real );
		if ( !simulated_names_.empty() )
		{
			out.key( "simulated" ).beginArray();
			for ( const auto& name : simulated_names_ )
			{
				out.value( name );
			}
			out.endArray();
		}
	}

	Result<> Daemon::developerMode() const
	{
		if ( config()->developer )
		{
			return {};
		}
		return fail( "developer mode is off: turn it on in Lexiglance (About: click the version seven times), with lexiglance --developer, or with \"developer\": true in its settings file" );
	}

	IpcServer::Handler Daemon::developerOnly( IpcServer::Handler handler ) const
	{
		return [this, handler = std::move( handler )]( const json::Value& params ) -> Result<std::string> {
			if ( auto on = developerMode(); !on )
			{
				return std::unexpected( on.error() );
			}
			return handler( params );
		};
	}

	void Daemon::registerDeveloperHandlers()
	{
		ipc_.guardTopics( [this] { return developerMode(); } );
		registerSimulationHandlers();

		// The log as it is written, to the clients that subscribed to it.
		log::setTap( [this]( log::Level level, std::string_view line ) {
			if ( ipc_.listening( IpcServer::Topic::Log ) )
			{
				json::Writer out;
				out.beginObject().field( "level", log::levelName( level ) ).field( "line", line ).endObject();
				ipc_.publish( IpcServer::Topic::Log, "log.line", out.str() );
			}
		} );

		ipc_.on( "debug.log", developerOnly( []( const json::Value& params ) -> Result<std::string> {
					 const auto   count = static_cast<std::size_t>( std::clamp<std::int64_t>( params["lines"].asInt( 500 ), 0, 20000 ) );
					 json::Writer out;
					 out.beginObject().field( "level", log::levelName( log::level() ) ).key( "lines" ).beginArray();
					 for ( const auto& line : report::tail( paths::stateDir() / "daemon.log", count ) )
					 {
						 out.value( line );
					 }
					 out.endArray().endObject();
					 return out.take();
				 } ) );

		ipc_.onSlow( "debug.capture", developerOnly( [this]( const json::Value& params ) -> Result<std::string> {
						 if ( !backend_ )
						 {
							 return fail( "no desktop integration available" );
						 }
						 // The capture thread's mailbox holds one request: a health check running meanwhile would take its place.
						 const std::scoped_lock lock( health_mutex_ );
						 auto                   request = std::make_shared<std::promise<Probe>>();
						 auto                   future  = request->get_future();
						 const platform::Point  point   = pointOf( params );
						 // Resolved like a real scan, so the capture clips to the window under the point.
						 backend_->post( [this, point, request] { capture_requests_.post( { .point = point, .window = backend_->windowAt( point ), .probe = request } ); } );
						 if ( future.wait_for( std::chrono::seconds( 10 ) ) != std::future_status::ready )
						 {
							 return fail( "capture timed out" );
						 }
						 const auto   probe = future.get();
						 json::Writer out;
						 out.beginObject().field( "text", probe.captured ? probe.captured->text : std::string() );
						 out.field( "took_ms", std::chrono::duration<double, std::milli>( probe.took ).count() ).field( "window", probe.window.wm_class );
						 if ( probe.captured )
						 {
							 const auto& captured = *probe.captured;
							 out.field( "confidence", static_cast<double>( captured.confidence ) )
									 .field( "source", captured.origin != nullptr ? std::string( captured.origin->name() ) : std::string() )
									 .field( "offset", captured.offset )
									 .field( "sentence", captured.sentence )
									 .field( "sentence_offset", captured.sentence_offset )
									 .field( "rewound", captured.rewound );
							 out.key( "character" );
							 writeRect( out, captured.character );
						 }
						 out.key( "lines" ).beginArray();
						 for ( const auto& line : probe.lines )
						 {
							 writeRect( out, line );
						 }
						 out.endArray();
						 if ( probe.snapshot )
						 {
							 const auto& shot = *probe.snapshot;
							 out.key( "ocr_lines" ).beginArray();
							 for ( const auto& line : shot.lines )
							 {
								 out.beginObject()
										 .field( "x", shot.region.x + line.box.x )
										 .field( "y", shot.region.y + line.box.y )
										 .field( "width", line.box.width )
										 .field( "height", line.box.height )
										 .field( "text", line.text )
										 .field( "loose", line.loose )
										 .endObject();
							 }
							 out.endArray();
							 if ( params["image"].asBool() )
							 {
								 const auto png = inspectionPng( shot, point, probe.lines, probe.captured ? probe.captured->character : platform::Rect{} );
								 out.key( "image" ).beginObject().field( "x", shot.region.x ).field( "y", shot.region.y ).field( "width", shot.region.width ).field( "height", shot.region.height ).field( "png", base64( png ) ).endObject();
							 }
						 }
						 out.endObject();
						 return out.take();
					 } ) );

		// Where the pointer is, for picking a point to capture or scan at.
		ipc_.onSlow( "debug.pointer", developerOnly( [this]( const json::Value& ) -> Result<std::string> {
						 if ( !backend_ )
						 {
							 return fail( "no desktop integration available" );
						 }
						 auto where  = std::make_shared<std::promise<platform::Point>>();
						 auto future = where->get_future();
						 backend_->post( [this, where] { where->set_value( backend_->pointer() ); } );
						 if ( future.wait_for( std::chrono::seconds( 2 ) ) != std::future_status::ready )
						 {
							 return fail( "desktop is not responding" );
						 }
						 const auto   point = future.get();
						 json::Writer out;
						 out.beginObject().field( "x", point.x ).field( "y", point.y ).endObject();
						 return out.take();
					 } ) );
	}

	void Daemon::registerSimulationHandlers()
	{
		// Exactly what holding the trigger there does: capture, look up, highlight and show the popup; `sentence`: with the
		// sentence key pressed as well.
		ipc_.on( "debug.scan", developerOnly( [this]( const json::Value& params ) -> Result<std::string> {
					 if ( !backend_ )
					 {
						 return fail( "no desktop integration available" );
					 }
					 const platform::Point point    = pointOf( params );
					 const bool            sentence = params["sentence"].asBool();
					 backend_->post( [this, point, sentence] { onScan( point, backend_->windowAt( point ), sentence, sentence ); } );
					 return "{}";
				 } ) );

		// Any text through the pipeline as a selection there is: looked up, translated as configured, the popup shown.
		ipc_.on( "debug.lookup", developerOnly( [this]( const json::Value& params ) -> Result<std::string> {
					 if ( !backend_ )
					 {
						 return fail( "no desktop integration available" );
					 }
					 if ( config()->paused )
					 {
						 return fail( "scanning is paused" );
					 }
					 std::string text( params["text"].asString() );
					 if ( text.empty() )
					 {
						 return fail( "missing \"text\"" );
					 }
					 const platform::Point point = pointOf( params );
					 backend_->post( [this, text = std::move( text ), point]() mutable { onSelection( std::move( text ), point ); } );
					 return "{}";
				 } ) );

		// A turn of the wheel over the popup on screen, as the backend reports one.
		ipc_.on( "debug.wheel", developerOnly( [this]( const json::Value& params ) -> Result<std::string> {
					 if ( !backend_ )
					 {
						 return fail( "no desktop integration available" );
					 }
					 const int delta = static_cast<int>( std::clamp<std::int64_t>( params["delta"].asInt( 1 ), -64, 64 ) );
					 backend_->post( [this, delta] { onAdjustLength( delta ); } );
					 return "{}";
				 } ) );

		// The trigger let go.
		ipc_.on( "debug.release", developerOnly( [this]( const json::Value& ) -> Result<std::string> {
					 if ( !backend_ )
					 {
						 return fail( "no desktop integration available" );
					 }
					 backend_->post( [this] { onTriggerReleased(); } );
					 return "{}";
				 } ) );

		// Problems made up for trying the settings application: in the health report and the status until set again (an
		// empty list) or the daemon restarts.
		ipc_.on( "debug.simulate", [this]( const json::Value& params ) -> Result<std::string> {
			// Taking them away is always allowed.
			if ( auto on = developerMode(); !on && !params["problems"].items().empty() )
			{
				return std::unexpected( on.error() );
			}
			std::vector<std::string>   names;
			std::vector<health::Check> checks;
			for ( const json::Value& item : params["problems"].items() )
			{
				auto made = simulatedProblem( item.asString() );
				if ( made.empty() )
				{
					return fail( "no such problem: {} (one of info, warning, error, ocr, permissions)", item.asString() );
				}
				names.emplace_back( item.asString() );
				std::ranges::move( made, std::back_inserter( checks ) );
			}
			{
				const std::scoped_lock lock( problems_mutex_ );
				simulated_names_ = names;
				simulated_       = std::move( checks );
			}
			std::string listed;
			for ( const auto& name : names )
			{
				listed.append( listed.empty() ? "" : ", " ).append( name );
			}
			log::info( "developer mode: simulated problems: {}", listed.empty() ? std::string( "none" ) : listed );
			ipc_.broadcast( "status.changed", statusJson() );
			return "{}";
		} );
	}

} // namespace lexiglance::daemon
