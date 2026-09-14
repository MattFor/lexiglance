#include <lexiglance/language/ja/Furigana.h>

#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/ja/Kana.h>

#include <algorithm>
#include <optional>

namespace lexiglance::lang::ja
{

	namespace
	{

		struct Group
		{
			std::u32string text;
			bool           kana = false;
		};

		std::u32string hiragana( std::u32string_view text )
		{
			std::u32string out( text );
			for ( char32_t& c : out )
			{
				c = toHiragana( c );
			}
			return out;
		}

		// Backtracking alignment: kana groups must match the reading literally, the others consume at least one character.
		bool align( const std::vector<Group>& groups, std::size_t group, std::u32string_view reading, std::vector<std::u32string>& out )
		{
			if ( group == groups.size() )
			{
				return reading.empty();
			}

			const Group& current = groups[group];
			if ( current.kana )
			{
				const auto normalized = hiragana( current.text );
				if ( !hiragana( reading ).starts_with( normalized ) )
				{
					return false;
				}
				out.emplace_back();
				if ( align( groups, group + 1, reading.substr( normalized.size() ), out ) )
				{
					return true;
				}
				out.pop_back();
				return false;
			}

			std::size_t reserved = 0;
			for ( std::size_t g = group + 1; g < groups.size(); ++g )
			{
				reserved += groups[g].kana ? groups[g].text.size() : 1;
			}
			for ( std::size_t length = 1; length + reserved <= reading.size(); ++length )
			{
				out.emplace_back( reading.substr( 0, length ) );
				if ( align( groups, group + 1, reading.substr( length ), out ) )
				{
					return true;
				}
				out.pop_back();
			}
			return false;
		}

	} // namespace

	std::vector<RubySegment> distributeFurigana( std::string_view expression, std::string_view reading )
	{
		const auto expr = utf8::toUtf32( expression );
		const auto read = utf8::toUtf32( reading );

		std::vector<Group> groups;
		for ( const char32_t c : expr )
		{
			const bool kana = isKana( c );
			if ( groups.empty() || groups.back().kana != kana )
			{
				groups.push_back( { .kana = kana } );
			}
			groups.back().text.push_back( c );
		}

		const bool has_kanji = std::ranges::any_of( groups, []( const Group& g ) { return !g.kana; } );
		if ( !has_kanji || reading.empty() || reading == expression )
		{
			return { { .text = std::string( expression ) } };
		}

		std::vector<std::u32string> readings;
		if ( !align( groups, 0, read, readings ) )
		{
			return { { .text = std::string( expression ), .reading = std::string( reading ) } };
		}

		std::vector<RubySegment> segments;
		segments.reserve( groups.size() );
		for ( std::size_t i = 0; i < groups.size(); ++i )
		{
			segments.push_back( { .text = utf8::fromUtf32( groups[i].text ), .reading = utf8::fromUtf32( readings[i] ) } );
		}
		return segments;
	}

} // namespace lexiglance::lang::ja
