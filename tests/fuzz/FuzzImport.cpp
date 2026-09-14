#include "FuzzFile.h"

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>

#include <string_view>

// The whole path of an untrusted dictionary: a Yomitan .zip is compiled, then opened and searched.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	const auto source = lexiglance::fuzz::write( data, size, ".zip" );
	auto       output = source;
	output.replace_extension( ".out.lgd" );
	if ( lexiglance::dict::compile( source, output ) )
	{
		if ( auto dictionary = lexiglance::dict::Dictionary::open( output ) )
		{
			constexpr std::string_view keys[] = { "a", "日本", "食べる" };
			for ( const std::string_view key : keys )
			{
				( void )( *dictionary )->findTerms( key );
			}
		}
	}
	return 0;
}
