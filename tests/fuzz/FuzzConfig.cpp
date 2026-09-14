#include <lexiglance/config/Config.h>
#include <lexiglance/core/Json.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	auto document = lexiglance::json::Document::parse( std::string( reinterpret_cast<const char*>( data ), size ) );
	if ( document )
	{
		if ( auto config = lexiglance::config::Config::fromJson( document->root() ) )
		{
			// Whatever was accepted must survive a round trip.
			auto again = lexiglance::json::Document::parse( config->toJson( false ) );
			if ( !again || !lexiglance::config::Config::fromJson( again->root() ) )
			{
				__builtin_trap();
			}
		}
	}
	return 0;
}
