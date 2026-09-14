#include <lexiglance/dictionary/StructuredContent.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	const auto                  sheet = lexiglance::dict::StyleSheet::parse( std::string_view( reinterpret_cast<const char*>( data ), size ) );
	lexiglance::dict::TextStyle style;
	constexpr std::string_view  tags[] = { "span", "div", "li", "td" };
	for ( const std::string_view tag : tags )
	{
		sheet.apply( tag, "gloss", "entry", style );
	}
	return 0;
}
