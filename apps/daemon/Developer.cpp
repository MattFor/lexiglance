#include "Developer.h"

#include <lexiglance/core/Log.h>

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace lexiglance::daemon
{

	namespace
	{

		// The value `share` of the way up the sorted samples (nearest rank).
		double percentile( std::vector<double> samples, double share )
		{
			if ( samples.empty() )
			{
				return 0.0;
			}
			const auto rank = static_cast<std::size_t>( std::ceil( share * static_cast<double>( samples.size() ) ) );
			const auto at   = samples.begin() + static_cast<std::ptrdiff_t>( std::clamp<std::size_t>( rank, 1, samples.size() ) - 1 );
			std::ranges::nth_element( samples, at );
			return *at;
		}

		cairo_status_t appendTo( void* closure, const unsigned char* data, unsigned int length )
		{
			static_cast<std::string*>( closure )->append( reinterpret_cast<const char*>( data ), length );
			return CAIRO_STATUS_SUCCESS;
		}

		std::vector<health::Check> madeUp( std::string_view name )
		{
			using health::Severity;
			constexpr std::string_view made_up = " (Simulated by developer mode: Developer -> Simulate.)";
			if ( name == "info" )
			{
				return { { .id = "simulated-info", .title = "Simulated notice", .status = Severity::Info, .detail = std::format( "Something worth knowing, with nothing to fix.{}", made_up ) } };
			}
			if ( name == "warning" )
			{
				return { { .id = "simulated-warning", .title = "Simulated warning", .status = Severity::Warning, .detail = std::format( "Something only partly works. Fix opens the Scanning page.{}", made_up ), .fix = "open-scanning" } };
			}
			if ( name == "error" )
			{
				return { { .id = "simulated-error", .title = "Simulated error", .status = Severity::Error, .detail = std::format( "Something doesn't work. Restart clears it: the restarted daemon forgets the simulation.{}", made_up ), .fix = "restart" } };
			}
			if ( name == "ocr" )
			{
				return { { .id     = "ocr",
				           .title  = "Text in images, games and videos (OCR)",
				           .status = Severity::Error,
				           .detail = std::format( "No OCR engine is installed: download PaddleOCR on the Scanning page.{}", made_up ),
				           .fix    = "open-scanning" } };
			}
			if ( name == "permissions" )
			{
				return {
					{ .id     = "accessibility",
					  .title  = "Text in applications (accessibility)",
					  .status = Severity::Error,
					  .detail = std::format( "Lexiglance doesn't have the Accessibility permission, so it can't read text in applications.{}", made_up ),
					  .fix    = "open-privacy-accessibility" },
					{ .id     = "input-monitoring",
					  .title  = "Input Monitoring",
					  .status = Severity::Error,
					  .detail = std::format( "macOS isn't letting Lexiglance see the keyboard, so it never sees the trigger.{}", made_up ),
					  .fix    = "open-privacy-input-monitoring" },
					{ .id     = "ocr",
					  .title  = "Text in images, games and videos (OCR)",
					  .status = Severity::Error,
					  .detail = std::format( "OCR can't read the screen without the Screen Recording permission.{}", made_up ),
					  .fix    = "open-privacy-screen-recording" },
				};
			}
			return {};
		}

	} // namespace

	std::string_view stageName( Stage stage ) noexcept
	{
		switch ( stage )
		{
			case Stage::Capture:
				return "capture";
			case Stage::Lookup:
				return "lookup";
			case Stage::Translate:
				return "translate";
			case Stage::Render:
				return "render";
			case Stage::Show:
				return "show";
		}
		return "?";
	}

	std::string base64( std::string_view data )
	{
		static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string                       out;
		out.reserve( ( ( data.size() + 2 ) / 3 ) * 4 );
		for ( std::size_t i = 0; i < data.size(); i += 3 )
		{
			const auto          byte = [&]( std::size_t k ) { return k < data.size() ? static_cast<std::uint32_t>( static_cast<unsigned char>( data[k] ) ) : 0U; };
			const std::uint32_t n    = ( byte( i ) << 16U ) | ( byte( i + 1 ) << 8U ) | byte( i + 2 );
			out.push_back( alphabet[( n >> 18U ) & 63U] );
			out.push_back( alphabet[( n >> 12U ) & 63U] );
			out.push_back( i + 1 < data.size() ? alphabet[( n >> 6U ) & 63U] : '=' );
			out.push_back( i + 2 < data.size() ? alphabet[n & 63U] : '=' );
		}
		return out;
	}

	std::string Tracer::event( std::uint64_t id, std::span<const Timing> timings, const Fields& fields )
	{
		json::Writer out;
		out.beginObject().field( "id", id ).field( "time", log::timestamp( std::chrono::system_clock::now() ).substr( 11 ) );
		out.key( "stages" ).beginObject();
		for ( const auto& [stage, took] : timings )
		{
			const double ms    = std::chrono::duration<double, std::milli>( took ).count();
			const auto   index = static_cast<std::size_t>( stage );
			double       p50   = 0.0;
			double       p95   = 0.0;
			{
				const std::scoped_lock lock( mutex_ );
				auto&                  kept_samples = samples_[index];
				if ( kept_samples.size() < kept )
				{
					kept_samples.push_back( ms );
				}
				else
				{
					kept_samples[next_[index]] = ms;
				}
				next_[index] = ( next_[index] + 1 ) % kept;
				p50          = percentile( kept_samples, 0.5 );
				p95          = percentile( kept_samples, 0.95 );
			}
			out.key( stageName( stage ) ).beginObject().field( "ms", ms ).field( "p50", p50 ).field( "p95", p95 ).endObject();
		}
		out.endObject();
		if ( fields )
		{
			fields( out );
		}
		out.endObject();
		return out.take();
	}

	void Tracer::send( std::uint64_t id, std::initializer_list<Timing> timings, const Fields& fields )
	{
		if ( on() )
		{
			ipc_->publish( IpcServer::Topic::Trace, "trace", event( id, timings, fields ) );
		}
	}

	std::vector<health::Check> simulatedProblem( std::string_view name )
	{
		auto checks = madeUp( name );
		for ( health::Check& check : checks )
		{
			check.simulated = true;
		}
		return checks;
	}

	void addSimulated( std::vector<health::Check>& checks, std::span<const health::Check> simulated )
	{
		for ( const health::Check& check : simulated )
		{
			const auto real = std::ranges::find( checks, check.id, &health::Check::id );
			if ( real != checks.end() )
			{
				*real = check;
			}
			else
			{
				checks.push_back( check );
			}
		}
	}

	std::string inspectionPng( const platform::Snapshot& snapshot, platform::Point pointer, std::span<const platform::Rect> text, const platform::Rect& character )
	{
		const int width  = snapshot.region.width;
		const int height = snapshot.region.height;
		if ( width <= 0 || height <= 0 || snapshot.rgb.size() != static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) * 3 )
		{
			return {};
		}
		cairo_surface_t* surface = cairo_image_surface_create( CAIRO_FORMAT_RGB24, width, height );
		cairo_surface_flush( surface );
		unsigned char* data   = cairo_image_surface_get_data( surface );
		const int      stride = cairo_image_surface_get_stride( surface );
		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				const std::size_t   at    = ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x ) ) * 3;
				const std::uint32_t pixel = ( static_cast<std::uint32_t>( snapshot.rgb[at] ) << 16U ) | ( static_cast<std::uint32_t>( snapshot.rgb[at + 1] ) << 8U ) | snapshot.rgb[at + 2];
				std::memcpy( data + ( static_cast<std::ptrdiff_t>( y ) * stride ) + ( static_cast<std::ptrdiff_t>( x ) * 4 ), &pixel, sizeof( pixel ) );
			}
		}
		cairo_surface_mark_dirty( surface );

		cairo_t*   cr  = cairo_create( surface );
		const auto box = [&]( const platform::Rect& rect, double r, double g, double b, double line ) {
			cairo_set_source_rgba( cr, r, g, b, 0.9 );
			cairo_set_line_width( cr, line );
			cairo_rectangle( cr, rect.x + 0.5, rect.y + 0.5, std::max( 1, rect.width - 1 ), std::max( 1, rect.height - 1 ) );
			cairo_stroke( cr );
		};
		const auto local = [&]( const platform::Rect& rect ) {
			return platform::Rect{ .x = rect.x - snapshot.region.x, .y = rect.y - snapshot.region.y, .width = rect.width, .height = rect.height };
		};
		for ( const auto& line : snapshot.lines )
		{
			const bool read = !line.text.empty();
			box( line.box, read ? 0.2 : 0.55, read ? 0.75 : 0.55, read ? 0.3 : 0.55, line.loose ? 1.0 : 1.5 );
		}
		for ( const platform::Rect& rect : text )
		{
			box( local( rect ), 1.0, 0.55, 0.0, 2.0 );
		}
		box( local( character ), 0.2, 0.45, 1.0, 2.0 );
		const double x = pointer.x - snapshot.region.x + 0.5;
		const double y = pointer.y - snapshot.region.y + 0.5;
		cairo_set_source_rgb( cr, 0.9, 0.1, 0.1 );
		cairo_set_line_width( cr, 1.0 );
		cairo_move_to( cr, x - 6, y );
		cairo_line_to( cr, x + 6, y );
		cairo_move_to( cr, x, y - 6 );
		cairo_line_to( cr, x, y + 6 );
		cairo_stroke( cr );
		cairo_destroy( cr );

		std::string png;
		cairo_surface_write_to_png_stream( surface, &appendTo, &png );
		cairo_surface_destroy( surface );
		return png;
	}

} // namespace lexiglance::daemon
