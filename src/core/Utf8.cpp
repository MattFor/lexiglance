#include <lexiglance/core/Utf8.h>

namespace lexiglance::utf8
{

	std::u32string toUtf32( std::string_view text )
	{
		std::u32string out;
		out.reserve( text.size() );
		for ( std::size_t pos = 0; pos < text.size(); )
		{
			out.push_back( decode( text, pos ) );
		}
		return out;
	}

	std::string fromUtf32( std::u32string_view text )
	{
		std::string out;
		out.reserve( text.size() * 3 );
		for ( const char32_t cp : text )
		{
			append( out, cp );
		}
		return out;
	}

} // namespace lexiglance::utf8
