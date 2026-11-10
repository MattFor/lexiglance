// The OCR benchmark's driver: shows each picture gen.py drew on a screen of its own making and points at its lines, as
// the daemon does at a real one, so everything between the pixels and the text is measured (finding the line,
// choosing the recogniser, reading on past the area first looked at, the confidence it takes). Prints a line of
// results for each line of truth: picture, language, font, style, size, error rate (0 to 1), confidence, what was
// wanted, what was read.
//   ocrbench <truth.tsv> <pictures directory> [--lookup] [--beside] [--languages ja,ru] [--engine tesseract]
//            [--shard 0/8] [--debug]
// --lookup reads as a lookup does rather than as a translation does (which wants the whole sentence); --beside points
// next to the text instead, where nothing should be read.
#include "ocr/OcrCapture.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <cairo.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{

	namespace lg = lexiglance;
	using lg::platform::Point;
	using lg::platform::Rect;

	// The picture on show, and where on the (otherwise empty) screen it is.
	struct Shown
	{
		int                       width  = 0;
		int                       height = 0;
		std::vector<std::uint8_t> rgb;
		Point                     origin;
	};

	bool show( Shown& shown, const std::string& path )
	{
		cairo_surface_t* png = cairo_image_surface_create_from_png( path.c_str() );
		const bool       ok  = cairo_surface_status( png ) == CAIRO_STATUS_SUCCESS;
		if ( ok )
		{
			shown.width      = cairo_image_surface_get_width( png );
			shown.height     = cairo_image_surface_get_height( png );
			const auto* data = cairo_image_surface_get_data( png );
			const auto  row  = static_cast<std::size_t>( cairo_image_surface_get_stride( png ) );
			const auto  wide = static_cast<std::size_t>( shown.width );
			shown.rgb.resize( wide * static_cast<std::size_t>( shown.height ) * 3 );
			for ( std::size_t y = 0; y < static_cast<std::size_t>( shown.height ); ++y )
			{
				for ( std::size_t x = 0; x < wide; ++x )
				{
					// Cairo's pixels are 0xAARRGGBB in the machine's byte order.
					std::uint32_t pixel = 0;
					std::copy_n( data + ( y * row ) + ( x * 4 ), 4, reinterpret_cast<unsigned char*>( &pixel ) );
					const auto at     = ( ( y * wide ) + x ) * 3;
					shown.rgb[at]     = static_cast<std::uint8_t>( pixel >> 16U );
					shown.rgb[at + 1] = static_cast<std::uint8_t>( pixel >> 8U );
					shown.rgb[at + 2] = static_cast<std::uint8_t>( pixel );
				}
			}
		}
		cairo_surface_destroy( png );
		return ok;
	}

	class Screen final : public lg::platform::ScreenReader
	{
	public:
		explicit Screen( const Shown* shown ) :
			shown_( shown )
		{
		}

		Rect bounds() override
		{
			return { .x = shown_->origin.x, .y = shown_->origin.y, .width = shown_->width, .height = shown_->height };
		}

		std::optional<Rect> windowRect( std::uint64_t /*id*/ ) override
		{
			return std::nullopt;
		}

		// Regions asked for lie within bounds().
		std::vector<std::uint8_t> read( const Rect& region ) override
		{
			std::vector<std::uint8_t> out( static_cast<std::size_t>( region.width ) * static_cast<std::size_t>( region.height ) * 3 );
			const auto                x = static_cast<std::ptrdiff_t>( region.x - shown_->origin.x );
			for ( int y = 0; y < region.height; ++y )
			{
				const auto from = ( ( static_cast<std::ptrdiff_t>( region.y - shown_->origin.y + y ) * shown_->width ) + x ) * 3;
				std::copy_n( shown_->rgb.begin() + from, static_cast<std::ptrdiff_t>( region.width ) * 3, out.begin() + ( static_cast<std::ptrdiff_t>( y ) * region.width * 3 ) );
			}
			return out;
		}

	private:
		const Shown* shown_;
	};

	// The letters compared: without spaces, case, Greek accents (all-caps fonts drop them, and lookups fold both) or
	// the width of punctuation.
	std::u32string letters( std::string_view text )
	{
		static const std::u32string from = U"ΆΈΉΊΌΎΏάέήίόύώϊϋΐΰς？！，．：；（）";
		static const std::u32string to   = U"αεηιουωαεηιουωιυιυσ?!,.:;()";
		std::u32string              out;
		for ( char32_t c : lg::utf8::codepoints( text ) )
		{
			if ( c == U' ' || c == 0x3000 )
			{
				continue;
			}
			if ( const auto at = from.find( c ); at != std::u32string::npos )
			{
				c = to[at];
			}
			if ( ( c >= U'A' && c <= U'Z' ) || ( c >= 0x410 && c <= 0x42F ) || ( c >= 0x391 && c <= 0x3A9 ) )
			{
				c += 0x20;
			}
			else if ( c >= 0x400 && c <= 0x40F )
			{
				c += 0x50;
			}
			out.push_back( c );
		}
		return out;
	}

	std::size_t editDistance( const std::u32string& a, const std::u32string& b )
	{
		std::vector<std::size_t> row( b.size() + 1 );
		std::ranges::iota( row, std::size_t{ 0 } );
		for ( std::size_t i = 1; i <= a.size(); ++i )
		{
			std::size_t diagonal = row[0];
			row[0]               = i;
			for ( std::size_t j = 1; j <= b.size(); ++j )
			{
				const std::size_t above = row[j];
				row[j]                  = std::min( { row[j] + 1, row[j - 1] + 1, diagonal + ( a[i - 1] == b[j - 1] ? 0 : 1 ) } );
				diagonal                = above;
			}
		}
		return row[b.size()];
	}

	std::vector<std::string> split( std::string_view text, char separator )
	{
		std::vector<std::string> out;
		for ( const auto part : std::views::split( text, separator ) )
		{
			out.emplace_back( std::string_view( part ) );
		}
		return out;
	}

	int number( std::string_view text )
	{
		int value = 0;
		std::from_chars( text.data(), text.data() + text.size(), value );
		return value;
	}

	struct Options
	{
		std::string truth;
		std::string pictures;
		std::string languages;
		bool        tesseract = false;
		bool        lookup    = false;
		bool        beside    = false;
		bool        debug     = false;
		int         shard     = 0;
		int         shards    = 1;
	};

	std::optional<Options> parse( const std::vector<std::string_view>& arguments )
	{
		Options                       options;
		std::vector<std::string_view> plain;
		for ( std::size_t i = 0; i < arguments.size(); ++i )
		{
			const std::string_view argument = arguments[i];
			const std::string_view value    = i + 1 < arguments.size() ? arguments[i + 1] : std::string_view();
			if ( argument == "--lookup" )
			{
				options.lookup = true;
			}
			else if ( argument == "--beside" )
			{
				options.beside = true;
			}
			else if ( argument == "--debug" )
			{
				options.debug = true;
			}
			else if ( argument == "--languages" )
			{
				options.languages = value;
				++i;
			}
			else if ( argument == "--engine" )
			{
				options.tesseract = value == "tesseract";
				++i;
			}
			else if ( argument == "--shard" )
			{
				const auto parts = split( value, '/' );
				options.shard    = parts.empty() ? 0 : number( parts.front() );
				options.shards   = std::max( 1, parts.size() > 1 ? number( parts[1] ) : 1 );
				++i;
			}
			else if ( argument.starts_with( "--" ) )
			{
				return std::nullopt;
			}
			else
			{
				plain.push_back( argument );
			}
		}
		if ( plain.size() != 2 )
		{
			return std::nullopt;
		}
		options.truth    = plain[0];
		options.pictures = plain[1];
		return options;
	}

	// Where a line is pointed at: a third and two thirds along it (the second is where the start of a long line is
	// out of the area read first), or beside it.
	Point pointAt( const Shown& shown, const Rect& line, bool far, bool beside )
	{
		Point point{ .x = shown.origin.x + line.x + ( ( line.width * ( far ? 7 : 3 ) ) / 10 ), .y = shown.origin.y + line.y + ( line.height / 2 ) };
		if ( beside && far )
		{
			point.x = std::min( shown.origin.x + shown.width - 2, shown.origin.x + line.x + line.width + 120 );
		}
		else if ( beside )
		{
			point.y = std::min( shown.origin.y + shown.height - 2, shown.origin.y + line.y + line.height + 45 );
		}
		return point;
	}

} // namespace

int main( int argc, char** argv )
{
	const auto options = parse( { argv + 1, argv + argc } );
	if ( !options )
	{
		std::println( stderr, "usage: ocrbench <truth.tsv> <pictures directory> [--lookup] [--beside] [--languages ja,ru] [--engine tesseract] [--shard 0/8] [--debug]" );
		return 2;
	}
	if ( options->debug )
	{
		lg::log::setLevel( lg::log::Level::Debug );
	}

	Shown                    shown;
	lg::platform::OcrOptions ocr;
	ocr.engine = options->tesseract ? lg::config::OcrEngine::Tesseract : lg::config::OcrEngine::Paddle;
	ocr.screen = [&]() -> lg::Result<std::unique_ptr<lg::platform::ScreenReader>> { return std::make_unique<Screen>( &shown ); };
	for ( const std::string& code : split( options->languages, ',' ) )
	{
		if ( const auto* language = lg::lang::findLanguage( code ) )
		{
			ocr.languages.push_back( language );
		}
	}
	auto capture = lg::platform::createOcrCapture( ocr );
	if ( !capture )
	{
		std::println( stderr, "error: {}", capture.error().message );
		return 1;
	}
	std::println( stderr, "{}", ( *capture )->describe() );

	std::ifstream in( options->truth );
	std::string   row;
	std::string   current;
	int           picture = 0;
	bool          mine    = false;
	const auto    began   = std::chrono::steady_clock::now();
	while ( std::getline( in, row ) )
	{
		// picture, the line's x, y, width and height, language, font, style, size, text
		const auto field = split( row, '\t' );
		if ( field.size() < 10 )
		{
			continue;
		}
		if ( field[0] != current )
		{
			current = field[0];
			++picture;
			// A place of its own for each picture, on the grid the areas read lie on: nothing kept from the picture
			// before it is taken for it.
			shown.origin = { .x = picture * 3840, .y = 0 };
			mine         = picture % options->shards == options->shard && show( shown, options->pictures + "/" + current );
		}
		if ( !mine )
		{
			continue;
		}
		const Rect  line{ .x = number( field[1] ), .y = number( field[2] ), .width = number( field[3] ), .height = number( field[4] ) };
		const auto  wanted = letters( field[9] );
		std::string read   = "<nothing>";
		double      worst  = -1.0;
		float       sure   = 0.0F;
		for ( const bool far : { false, true } )
		{
			const auto   found = ( *capture )->capture( pointAt( shown, line, far, options->beside ), {}, { .characters = 20, .sentence = !options->lookup } );
			const auto   got   = found ? letters( found->sentence ) : std::u32string();
			const double error = std::min( 1.0, static_cast<double>( editDistance( got, wanted ) ) / static_cast<double>( std::max<std::size_t>( 1, wanted.size() ) ) );
			if ( error > worst )
			{
				worst = error;
				read  = found ? found->sentence : std::string( "<nothing>" );
				sure  = found ? found->confidence : 0.0F;
			}
		}
		std::println( "{}\t{}\t{}\t{}\t{}\t{:.3f}\t{:.0f}\t{}\t{}", field[0], field[5], field[6], field[7], field[8], worst, sure, field[9], read );
	}
	std::println( stderr, "shard {}/{}: {} s", options->shard, options->shards, std::chrono::duration_cast<std::chrono::seconds>( std::chrono::steady_clock::now() - began ).count() );
	return 0;
}
