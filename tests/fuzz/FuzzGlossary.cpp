#include <lexiglance/dictionary/StructuredContent.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The first byte picks the glossary kind, the second the markup; the rest is the glossary as a dictionary stores it.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	if ( size < 2 )
	{
		return 0;
	}
	namespace dict           = lexiglance::dict;
	const auto          kind = static_cast<dict::format::GlossKind>( data[0] % 4U );
	dict::MarkupOptions options;
	options.format = static_cast<dict::Markup>( data[1] % 3U );
	std::string out;
	dict::renderGlossary( out, kind, std::string_view( reinterpret_cast<const char*>( data ) + 2, size - 2 ), options );
	return 0;
}
