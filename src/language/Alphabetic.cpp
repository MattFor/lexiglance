#include <lexiglance/language/Alphabetic.h>

#include "Variants.h"

#include <lexiglance/language/Case.h>

#include <algorithm>

namespace lexiglance::lang
{

	bool AlphabeticLanguage::isLookupCharacter( char32_t c ) const noexcept
	{
		// Hyphens join words (кто-нибудь) and apostrophes are part of some (aujourd'hui).
		return isScriptCharacter( c ) || isStressMark( c ) || c == U'-' || c == U'‐' || c == U'‑' || c == U'\'' || c == U'’' || c == U'ʼ';
	}

	const Fold* AlphabeticLanguage::foldOf( char32_t c ) const noexcept
	{
		const auto folds = this->folds();
		const auto it    = std::ranges::find( folds, c, &Fold::first );
		return it != folds.end() ? &*it : nullptr;
	}

	void AlphabeticLanguage::variants( std::u32string_view source, std::vector<TextVariant>& out ) const
	{
		// Stress marks go, the letter before them covering them.
		Units base;
		base.reserve( source.size() );
		for ( std::size_t i = 0; i < source.size(); ++i )
		{
			const auto end = static_cast<std::uint32_t>( i + 1 );
			if ( isStressMark( source[i] ) && !base.empty() )
			{
				base.back().second = end;
				continue;
			}
			base.emplace_back( source[i], end );
		}

		std::vector<Units> spellings{ base };
		const auto         capitals = std::ranges::count_if( base, []( const auto& unit ) { return isUppercase( unit.first ); } );
		if ( capitals > 0 )
		{
			spellings.push_back( mapUnits( base, []( char32_t c ) { return lowercase( c ); } ) );
		}
		if ( capitals > 1 )
		{
			Units capitalised         = spellings.back();
			capitalised.front().first = uppercase( capitalised.front().first );
			spellings.push_back( std::move( capitalised ) );
		}

		const std::size_t written = spellings.size();
		for ( std::size_t i = 0; i < written; ++i )
		{
			if ( std::ranges::any_of( spellings[i], [this]( const auto& unit ) { return foldOf( unit.first ) != nullptr; } ) )
			{
				// Folded letters become their fold; those folded into nothing go, the letter before covering them.
				Units folded;
				for ( const auto& unit : spellings[i] )
				{
					const Fold* fold = foldOf( unit.first );
					if ( fold == nullptr || fold->second != 0 )
					{
						folded.emplace_back( fold != nullptr ? fold->second : unit.first, unit.second );
					}
					else if ( !folded.empty() )
					{
						folded.back().second = unit.second;
					}
				}
				if ( !folded.empty() )
				{
					spellings.push_back( std::move( folded ) );
				}
			}
		}

		for ( const Units& units : spellings )
		{
			addVariant( out, units );
		}
	}

} // namespace lexiglance::lang
