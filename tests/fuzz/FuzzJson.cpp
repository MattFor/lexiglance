#include <lexiglance/core/Json.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	auto document = lexiglance::json::Document::parse( std::string( reinterpret_cast<const char*>( data ), size ) );
	if ( document )
	{
		std::size_t count = 0;
		for ( const auto& item : document->root().items() )
		{
			count += item.asString().size();
		}
		( void )count;
	}
	return 0;
}
