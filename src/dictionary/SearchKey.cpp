#include <lexiglance/dictionary/SearchKey.h>

#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace lexiglance::dict
{

	namespace
	{

		// A character spelled another way in search keys: stress marks go, and the letters languages fold (ё as е, ά as
		// α) become the letter they fold to.
		struct Fold
		{
			std::string from;
			std::string to;
		};

		struct Folds
		{
			std::vector<Fold> folds;
			// The first bytes of the folded characters, so most text is passed over without a search.
			std::array<bool, 256> leads{};
		};

		const Folds& table()
		{
			static const Folds folds = [] {
				Folds      made;
				const auto add = [&made]( char32_t from, char32_t to ) {
					std::string source;
					utf8::append( source, from );
					if ( from == to || std::ranges::any_of( made.folds, [&]( const Fold& fold ) { return fold.from == source; } ) )
					{
						return;
					}
					std::string target;
					if ( to != 0 )
					{
						utf8::append( target, to );
					}
					made.leads[static_cast<std::uint8_t>( source.front() )] = true;
					made.folds.push_back( { .from = std::move( source ), .to = std::move( target ) } );
				};
				add( 0x0300, 0 );
				add( 0x0301, 0 );
				for ( const lang::Language* language : lang::languages() )
				{
					for ( const auto& [from, to] : language->folds() )
					{
						add( from, to );
					}
				}
				return made;
			}();
			return folds;
		}

		// The fold of the character starting at `pos`, if it has one.
		const Fold* foldAt( std::string_view text, std::size_t pos ) noexcept
		{
			const Folds& folds = table();
			if ( !folds.leads[static_cast<std::uint8_t>( text[pos] )] )
			{
				return nullptr;
			}
			const auto rest = text.substr( pos );
			const auto it   = std::ranges::find_if( folds.folds, [&]( const Fold& fold ) { return rest.starts_with( fold.from ); } );
			return it != folds.folds.end() ? &*it : nullptr;
		}

		bool isStressMark( const Fold* fold ) noexcept
		{
			return fold != nullptr && fold->to.empty();
		}

	} // namespace

	std::string searchKey( std::string_view text )
	{
		std::size_t first = 0;
		while ( first < text.size() && foldAt( text, first ) == nullptr )
		{
			++first;
		}
		if ( first == text.size() )
		{
			return {};
		}

		std::string key( text.substr( 0, first ) );
		for ( std::size_t pos = first; pos < text.size(); )
		{
			if ( const Fold* fold = foldAt( text, pos ) )
			{
				key.append( fold->to );
				pos += fold->from.size();
				continue;
			}
			key.push_back( text[pos++] );
		}
		return key;
	}

	bool matchesSearchKey( std::string_view text, std::string_view key ) noexcept
	{
		std::size_t at = 0;
		for ( std::size_t pos = 0; pos < text.size(); )
		{
			if ( const Fold* fold = foldAt( text, pos ) )
			{
				if ( !key.substr( at ).starts_with( fold->to ) )
				{
					return false;
				}
				at += fold->to.size();
				pos += fold->from.size();
				continue;
			}
			if ( at >= key.size() || key[at] != text[pos] )
			{
				return false;
			}
			++at;
			++pos;
		}
		return at == key.size();
	}

	bool onlyAddsStress( std::string_view reading, std::string_view expression ) noexcept
	{
		bool        stressed = false;
		std::size_t at       = 0;
		for ( std::size_t pos = 0; pos < reading.size(); )
		{
			if ( const Fold* fold = foldAt( reading, pos ); isStressMark( fold ) )
			{
				stressed = true;
				pos += fold->from.size();
				continue;
			}
			if ( at >= expression.size() || expression[at] != reading[pos] )
			{
				return false;
			}
			++at;
			++pos;
		}
		return stressed && at == expression.size();
	}

} // namespace lexiglance::dict
