#include <lexiglance/ipc/Protocol.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	lexiglance::ipc::LineBuffer buffer;
	std::size_t                 at = 0;
	while ( at < size && !buffer.overflowed() )
	{
		// Pieces of varying size, as a socket delivers them.
		const std::size_t piece = std::min<std::size_t>( size - at, 1U + ( data[at] % 64U ) );
		buffer.append( std::string_view( reinterpret_cast<const char*>( data ) + at, piece ) );
		at += piece;
		while ( auto line = buffer.next() )
		{
			if ( line->find( '\n' ) != std::string::npos )
			{
				__builtin_trap();
			}
		}
	}
	return 0;
}
