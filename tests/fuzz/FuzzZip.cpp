#include "FuzzFile.h"

#include <lexiglance/core/Zip.h>

extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	const auto path = lexiglance::fuzz::write( data, size, ".zip" );
	if ( auto archive = lexiglance::ZipArchive::open( path ) )
	{
		for ( const auto& entry : archive->entries() )
		{
			// Huge declared sizes are refused by the reader; only the allocation would slow the fuzzer down.
			if ( entry.size < ( 16U << 20U ) )
			{
				( void )archive->read( entry );
			}
		}
	}
	return 0;
}
