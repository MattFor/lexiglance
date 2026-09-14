#include "FuzzFile.h"

#include <lexiglance/dictionary/Dictionary.h>

#include <string_view>

// Compiled dictionaries are memory mapped and trusted only as far as their own header and section sizes go.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	const auto path = lexiglance::fuzz::write( data, size, ".lgd" );
	if ( auto dictionary = lexiglance::dict::Dictionary::open( path ) )
	{
		constexpr std::string_view keys[] = { "", "a", "日本", "たべる", "食べる" };
		for ( const std::string_view key : keys )
		{
			( void )( *dictionary )->findTerms( key );
		}
	}
	return 0;
}
