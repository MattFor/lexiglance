#ifndef LEXIGLANCE_LANGUAGE_VARIANTS_H
#define LEXIGLANCE_LANGUAGE_VARIANTS_H

#include <lexiglance/language/Language.h>

#include <cstdint>
#include <utility>
#include <vector>

// Helpers for Language::variants: a spelling as code points, each with how much of the source it covers.
namespace lexiglance::lang
{

	// Code points of a spelling, each with the number of source code points up to and including it.
	using Units = std::vector<std::pair<char32_t, std::uint32_t>>;

	template <typename F>
	[[nodiscard]] Units mapUnits( const Units& in, const F& transform )
	{
		Units out = in;
		for ( auto& unit : out )
		{
			unit.first = transform( unit.first );
		}
		return out;
	}

	[[nodiscard]] TextVariant toVariant( const Units& units );

	// Appends the spelling unless an equal one is there already.
	void addVariant( std::vector<TextVariant>& out, const Units& units );

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_VARIANTS_H
