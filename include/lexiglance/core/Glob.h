#ifndef LEXIGLANCE_CORE_GLOB_H
#define LEXIGLANCE_CORE_GLOB_H

#include <cstddef>
#include <string_view>

namespace lexiglance
{

	// Case-insensitive (ASCII) wildcard match supporting `*` and `?`.
	[[nodiscard]] constexpr bool globMatch( std::string_view pattern, std::string_view text ) noexcept
	{
		const auto lower = []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; };

		std::size_t p      = 0;
		std::size_t t      = 0;
		std::size_t star   = std::string_view::npos;
		std::size_t resume = 0;
		while ( t < text.size() )
		{
			if ( p < pattern.size() && ( pattern[p] == '?' || lower( pattern[p] ) == lower( text[t] ) ) )
			{
				++p;
				++t;
			}
			else if ( p < pattern.size() && pattern[p] == '*' )
			{
				star   = p++;
				resume = t;
			}
			else if ( star != std::string_view::npos )
			{
				p = star + 1;
				t = ++resume;
			}
			else
			{
				return false;
			}
		}
		while ( p < pattern.size() && pattern[p] == '*' )
		{
			++p;
		}
		return p == pattern.size();
	}

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_GLOB_H
