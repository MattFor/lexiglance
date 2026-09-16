#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/dictionary/DictionaryStore.h>
#include <lexiglance/dictionary/StructuredContent.h>
#include <lexiglance/ipc/Socket.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/Translator.h>
#include <lexiglance/ocr/Paddle.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
	#include <io.h>
#else
	#include <unistd.h>
#endif

namespace
{

	// Whether errors go to a terminal (and may be coloured).
	bool terminal()
	{
#ifdef _WIN32
		return ::_isatty( ::_fileno( stderr ) ) != 0;
#else
		return ::isatty( STDERR_FILENO ) != 0;
#endif
	}

	namespace lg = lexiglance;
	using Args   = std::vector<std::string_view>;

	void usage()
	{
		std::println( "lexiglancectl {} - scripting client for the Lexiglance dictionary", lg::version );
		std::println( "" );
		std::println( "usage: lexiglancectl <command> [arguments]" );
		std::println( "" );
		std::println( "  import <dictionary.zip|directory>... [--replace]   compile Yomitan dictionaries" );
		std::println( "  list                                              list installed dictionaries" );
		std::println( "  remove <title>                                    uninstall a dictionary" );
		std::println( "  lookup <text>                                     look up the start of <text>" );
		std::println( "  bench <text> [iterations]                         measure lookup latency" );
		std::println( "  ocr <image.ppm>                                   read the text in an image with PaddleOCR" );
		std::println( "" );
		std::println( "  status | pause | resume | reload | hide           control the running daemon" );
		std::println( "  show <text>                                       show a popup at the pointer" );
		std::println( "  health                                            check that everything works (exit 1 on problems)" );
		std::println( "  stats [reset]                                     what has been looked up so far, or forget it" );
	}

	// A detail of several lines, with the later ones under the first instead of against the margin.
	std::string indented( std::string_view detail )
	{
		std::string out;
		for ( const char c : detail )
		{
			out.push_back( c );
			if ( c == '\n' )
			{
				out.append( 9, ' ' );
			}
		}
		return out;
	}

	// Prints the daemon's self-checks, one per line; exits with 1 when any failed.
	int healthCommand()
	{
		auto client = lg::ipc::Client::connect( lg::paths::ipcEndpoint(), std::chrono::seconds( 30 ) );
		if ( !client )
		{
			std::println( stderr, "error: the daemon is not running ({})", client.error().message );
			return 1;
		}
		const auto reply = client->call( "health" );
		if ( !reply )
		{
			std::println( stderr, "error: {}", reply.error().message );
			return 1;
		}
#ifdef _WIN32
		const bool colour = false;
#else
		const bool colour = ::isatty( STDOUT_FILENO ) != 0;
#endif
		int errors = 0;
		for ( const auto& check : lg::health::read( reply->root()["result"] ) )
		{
			std::string_view mark  = "ok";
			std::string_view paint = "\x1b[32m";
			switch ( check.status )
			{
				case lg::health::Severity::Info:
					mark  = "info";
					paint = "\x1b[36m";
					break;
				case lg::health::Severity::Warning:
					mark  = "warning";
					paint = "\x1b[33m";
					break;
				case lg::health::Severity::Error:
					mark  = "error";
					paint = "\x1b[31m";
					++errors;
					break;
				case lg::health::Severity::Ok:
					break;
			}
			std::println( "{}{:<8}{} {}: {}", colour ? paint : "", mark, colour ? "\x1b[0m" : "", check.title, indented( check.detail ) );
			if ( !check.fix.empty() )
			{
				std::println( "         fix: {}", check.fix );
			}
		}
		return errors > 0 ? 1 : 0;
	}

	// Sends one request to the running daemon and prints the result's fields.
	int call( std::string_view method, const std::string& params = "{}" )
	{
		auto client = lg::ipc::Client::connect( lg::paths::ipcEndpoint() );
		if ( !client )
		{
			std::println( stderr, "error: the daemon is not running ({})", client.error().message );
			return 1;
		}
		const auto reply = client->call( method, params );
		if ( !reply )
		{
			std::println( stderr, "error: {}", reply.error().message );
			return 1;
		}
		for ( const auto& [key, value] : reply->root()["result"].members() )
		{
			std::println( "{}: {}", key, value.isString() ? std::string( value.asString() ) : lg::json::serialize( value ) );
		}
		return 0;
	}

	// Lets a running daemon pick up dictionaries changed behind its back.
	void notifyReload()
	{
		if ( auto client = lg::ipc::Client::connect( lg::paths::ipcEndpoint() ) )
		{
			( void )client->call( "dictionaries.reload" );
		}
	}

	std::shared_ptr<const lg::lookup::DictionarySet> loadSet()
	{
		const lg::dict::DictionaryStore store( lg::paths::dictionariesDir() );
		std::vector<lg::Error>          errors;
		auto                            dictionaries = store.loadAll( &errors );
		for ( const auto& error : errors )
		{
			std::println( stderr, "warning: {}", error.message );
		}

		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.reserve( dictionaries.size() );
		for ( auto& dictionary : dictionaries )
		{
			auto styles = std::make_shared<const lg::dict::StyleSheet>( lg::dict::StyleSheet::parse( dictionary->styles() ) );
			auto name   = dictionary->info().title;
			dictionary->prefetchIndexes();
			loaded.push_back( { .dictionary = std::move( dictionary ), .styles = std::move( styles ), .name = std::move( name ) } );
		}
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	int importCommand( const Args& args )
	{
		const bool                      replace = std::ranges::find( args, std::string_view( "--replace" ) ) != args.end();
		const lg::dict::DictionaryStore store( lg::paths::dictionariesDir() );
		int                             failures = 0;

		for ( const std::string_view path : args )
		{
			if ( path.starts_with( "--" ) )
			{
				continue;
			}

			lg::dict::ImportOptions options;
			// Progress is redrawn in place, which only makes sense on a terminal.
			if ( terminal() )
			{
				options.on_progress = []( const lg::dict::ImportProgress& progress ) {
					if ( progress.total > 0 )
					{
						std::print( stderr, "\r  {} {}/{}   ", progress.stage, progress.done, progress.total );
					}
				};
			}

			const auto summary = store.install( std::filesystem::path( path ), options, replace );
			if ( terminal() )
			{
				std::print( stderr, "\r\033[K" );
			}
			if ( !summary )
			{
				std::println( stderr, "error: {}: {}", path, summary.error().message );
				++failures;
				continue;
			}
			std::println(
					"{}: {} terms, {} meta, {} kanji, {} tags, {:.1f} MiB in {:.2f}s",
					summary->title,
					summary->terms,
					summary->meta,
					summary->kanji,
					summary->tags,
					static_cast<double>( summary->bytes ) / ( 1024.0 * 1024.0 ),
					summary->seconds
			);
		}
		notifyReload();
		return failures == 0 ? 0 : 1;
	}

	int listCommand()
	{
		const lg::dict::DictionaryStore store( lg::paths::dictionariesDir() );
		for ( const auto& dictionary : store.loadAll() )
		{
			const auto& info = dictionary->info();
			std::println(
					"{:<32} {:>8} terms {:>8} meta {:>6} kanji  {}",
					info.title,
					dictionary->terms().size(),
					dictionary->meta().size(),
					dictionary->kanji().size(),
					info.revision
			);
		}
		return 0;
	}

	int removeCommand( const Args& args )
	{
		if ( args.empty() )
		{
			usage();
			return 2;
		}
		const lg::dict::DictionaryStore store( lg::paths::dictionariesDir() );
		if ( const auto removed = store.remove( args.front() ); !removed )
		{
			std::println( stderr, "error: {}", removed.error().message );
			return 1;
		}
		notifyReload();
		return 0;
	}

	void printResult( const lg::lookup::LookupResult& result )
	{
		const lg::dict::MarkupOptions markup{ .format = lg::dict::Markup::Plain };
		const auto&                   set = *result.dictionaries;

		for ( const auto& entry : result.terms )
		{
			std::print( "{} [{}]  matched \"{}\"", entry.expression, entry.reading, result.matchedText( entry.matched_length ) );
			if ( const auto inflection = result.inflectionText( entry ); !inflection.empty() )
			{
				std::print( "  ({})", inflection );
			}
			std::println( "" );

			for ( const auto& frequency : entry.frequencies )
			{
				std::println( "    freq {}: {}", set[frequency.dictionary].name, frequency.display.empty() ? std::to_string( frequency.value ) : std::string( frequency.display ) );
			}
			for ( const auto& definition : entry.definitions )
			{
				const auto& dictionary = *set[definition.dictionary].dictionary;
				const auto& term       = dictionary.terms()[definition.term];
				std::println( "    {} [{}]", set[definition.dictionary].name, dictionary.string( term.definition_tags ) );
				for ( const auto& gloss : dictionary.glossary( term ) )
				{
					std::string text;
					lg::dict::renderGlossary( text, gloss.kind, dictionary.string( gloss.data ), markup );
					std::println( "      - {}", text );
				}
			}
		}
		for ( const auto& kanji : result.kanji )
		{
			const auto& dictionary = *set[kanji.dictionary].dictionary;
			std::println(
					"kanji {}  on: {}  kun: {}",
					dictionary.string( kanji.record->character ),
					dictionary.string( kanji.record->onyomi ),
					dictionary.string( kanji.record->kunyomi )
			);
		}
		std::println( "{} terms in {} us", result.terms.size(), std::chrono::duration_cast<std::chrono::microseconds>( result.elapsed ).count() );
	}

	int lookupCommand( const Args& args, bool bench )
	{
		if ( args.empty() )
		{
			usage();
			return 2;
		}

		const auto             set = loadSet();
		lg::lookup::Translator translator;

		if ( !bench )
		{
			printResult( translator.lookup( set, args.front() ) );
			return 0;
		}

		int iterations = 10000;
		if ( args.size() > 1 )
		{
			const std::string_view count = args[1];
			const auto [end, error]      = std::from_chars( count.data(), count.data() + count.size(), iterations );
			if ( error != std::errc{} || end != count.data() + count.size() || iterations <= 0 )
			{
				std::println( stderr, "lexiglancectl: {} is not a number of lookups", count );
				return 2;
			}
		}
		( void )translator.lookup( set, args.front() );

		const auto  started = std::chrono::steady_clock::now();
		std::size_t results = 0;
		for ( int i = 0; i < iterations; ++i )
		{
			results += translator.lookup( set, args.front() ).terms.size();
		}
		const auto elapsed = std::chrono::duration<double, std::micro>( std::chrono::steady_clock::now() - started ).count();
		std::println( "{} lookups, {:.2f} us per lookup ({} results each)", iterations, elapsed / iterations, results / static_cast<std::size_t>( iterations ) );
		return 0;
	}

	// Binary netpbm (P6, 8 or 16 bits); anything converts to it with `magick input.png output.ppm`.
	std::optional<lg::ocr::Image> readPpm( const std::string& path )
	{
		std::ifstream in( path, std::ios::binary );
		std::string   magic;
		int           width   = 0;
		int           height  = 0;
		int           maximum = 0;
		in >> magic >> width >> height >> maximum;
		in.get();
		if ( !in || magic != "P6" || maximum <= 0 || maximum > 65535 || width <= 0 || height <= 0 )
		{
			return std::nullopt;
		}
		const std::size_t         samples = static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) * 3;
		const std::size_t         bytes   = maximum > 255 ? 2 : 1;
		std::vector<std::uint8_t> raw( samples * bytes );
		in.read( reinterpret_cast<char*>( raw.data() ), static_cast<std::streamsize>( raw.size() ) );
		if ( !in )
		{
			return std::nullopt;
		}
		lg::ocr::Image image{ .width = width, .height = height, .rgb = std::vector<std::uint8_t>( samples ) };
		const auto     top = static_cast<unsigned>( maximum );
		for ( std::size_t i = 0; i < samples; ++i )
		{
			const unsigned value = bytes == 2 ? ( static_cast<unsigned>( raw[2 * i] ) << 8U ) | raw[( 2 * i ) + 1] : raw[i];
			image.rgb[i]         = static_cast<std::uint8_t>( ( ( std::min( value, top ) * 255U ) + ( top / 2 ) ) / top );
		}
		return image;
	}

	int ocrCommand( const Args& args )
	{
		if ( args.empty() )
		{
			usage();
			return 2;
		}
		const auto image = readPpm( std::string( args.front() ) );
		if ( !image )
		{
			std::println( stderr, "error: expected a binary PPM image (magick input.png output.ppm)" );
			return 1;
		}
		auto ocr = lg::ocr::PaddleOcr::load( lg::paths::ocrDir() / "paddle", lg::paths::ocrDir() / "runtime", 4 );
		if ( !ocr )
		{
			std::println( stderr, "error: {}", ocr.error().message );
			return 1;
		}

		const auto started = std::chrono::steady_clock::now();
		const auto boxes   = ( *ocr )->detect( *image );
		const auto found   = std::chrono::steady_clock::now();
		std::println( "{} text boxes in {} ms", boxes.size(), std::chrono::duration_cast<std::chrono::milliseconds>( found - started ).count() );
		for ( const auto& box : boxes )
		{
			const auto  begin = std::chrono::steady_clock::now();
			const auto  line  = ( *ocr )->recognize( *image, box );
			std::string text;
			std::string positions;
			for ( const auto& c : line.characters )
			{
				text.append( c.text );
				positions.append( std::format( " {}[{:.0f}-{:.0f} {:.2f}]", c.text, c.from, c.to, c.confidence ) );
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - begin ).count();
			std::println( "{},{} {}x{} score {:.2f}{} ({} ms): {}", box.inner_x, box.inner_y, box.inner_width, box.inner_height, box.score, line.vertical ? " vertical" : "", ms, text );
			std::println( "   {}", positions );
		}
		return 0;
	}

	int run( std::span<char*> argv )
	{
		if ( argv.size() < 2 )
		{
			usage();
			return 2;
		}

		const std::string_view command = argv[1];
		const Args             args( argv.begin() + 2, argv.end() );

		if ( command == "import" )
		{
			return importCommand( args );
		}
		if ( command == "list" )
		{
			return listCommand();
		}
		if ( command == "remove" )
		{
			return removeCommand( args );
		}
		if ( command == "lookup" || command == "bench" )
		{
			return lookupCommand( args, command == "bench" );
		}
		if ( command == "ocr" )
		{
			return ocrCommand( args );
		}
		if ( command == "status" )
		{
			return call( "status" );
		}
		if ( command == "health" )
		{
			return healthCommand();
		}
		if ( command == "stats" )
		{
			if ( args.empty() )
			{
				return call( "stats" );
			}
			return args.front() == "reset" ? call( "stats.reset" ) : 2;
		}
		if ( command == "pause" || command == "resume" )
		{
			return call( "scan.pause", command == "pause" ? R"({"paused":true})" : R"({"paused":false})" );
		}
		if ( command == "reload" )
		{
			return call( "dictionaries.reload" );
		}
		if ( command == "hide" )
		{
			return call( "popup.hide" );
		}
		if ( command == "show" && !args.empty() )
		{
			lg::json::Writer params;
			params.beginObject().field( "text", args.front() ).endObject();
			return call( "popup.show", params.take() );
		}
		usage();
		return 2;
	}

} // namespace

int main( int argc, char** argv )
{
	try
	{
		return run( std::span<char*>( argv, static_cast<std::size_t>( argc ) ) );
	}
	catch ( const std::exception& e )
	{
		( void )std::fputs( "fatal: ", stderr );
		( void )std::fputs( e.what(), stderr );
		( void )std::fputs( "\n", stderr );
	}
	catch ( ... )
	{
		( void )std::fputs( "fatal: unknown error\n", stderr );
	}
	return 1;
}
