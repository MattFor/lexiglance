#include <lexiglance/core/Json.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/ipc/Messages.h>
#include <lexiglance/lookup/Translator.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace
{

	namespace lg = lexiglance;

	// The Japanese and Russian test dictionaries, compiled once per process.
	std::shared_ptr<const lg::lookup::DictionarySet> fixtures()
	{
		// tests/data from this file's path, not a define: CI's clang-tidy reads this file without the fuzz targets' flags.
		const auto                                data = std::filesystem::path( __FILE__ ).parent_path().parent_path() / "data";
		std::vector<lg::lookup::LoadedDictionary> loaded;
		for ( const char* name : { "fixture", "fixture-ru" } )
		{
			const auto output = std::filesystem::temp_directory_path() / ( "lexiglance-fuzz-" + std::to_string( ::getpid() ) + name + ".lgd" );
			if ( !lg::dict::compile( data / name, output ) )
			{
				__builtin_trap();
			}
			auto dictionary = lg::dict::Dictionary::open( output );
			if ( !dictionary )
			{
				__builtin_trap();
			}
			loaded.push_back( { .dictionary = *dictionary, .styles = nullptr, .name = name } );
		}
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

} // namespace

// Text from the screen, OCR or an IPC client, looked up and written out as the daemon answers a lookup request.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	static const auto      set = fixtures();
	lg::lookup::Translator translator;
	const std::string_view text( reinterpret_cast<const char*>( data ), size );
	const auto             result = translator.lookup( set, text, { .max_length = 24, .max_results = 16 } );
	for ( const auto& entry : result.terms )
	{
		( void )result.inflectionText( entry );
		( void )result.headword( entry );
	}
	lg::json::Writer out;
	lg::ipc::writeLookup( out, result, static_cast<lg::dict::Markup>( size % 3U ) );
	if ( !lg::json::Document::parse( out.str() ) )
	{
		__builtin_trap();
	}
	( void )translator.readings( set, text.substr( 0, 64 ), {} );
	return 0;
}
