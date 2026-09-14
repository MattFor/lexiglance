#ifndef LEXIGLANCE_CORE_UTF8_H
#define LEXIGLANCE_CORE_UTF8_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace lexiglance::utf8
{

	inline constexpr char32_t replacement_character = U'�';

	[[nodiscard]] constexpr std::size_t sequenceLength( unsigned char lead ) noexcept
	{
		if ( lead < 0x80 )
		{
			return 1;
		}
		if ( lead < 0xC2 )
		{
			return 0;
		}
		if ( lead < 0xE0 )
		{
			return 2;
		}
		if ( lead < 0xF0 )
		{
			return 3;
		}
		if ( lead < 0xF5 )
		{
			return 4;
		}
		return 0;
	}

	// Decodes the code point at `pos` and advances past it. Malformed input yields U+FFFD and advances one byte.
	[[nodiscard]] constexpr char32_t decode( std::string_view text, std::size_t& pos ) noexcept
	{
		const auto        lead   = static_cast<unsigned char>( text[pos] );
		const std::size_t length = sequenceLength( lead );

		if ( length == 1 )
		{
			++pos;
			return lead;
		}

		if ( length == 0 || pos + length > text.size() )
		{
			++pos;
			return replacement_character;
		}

		char32_t cp = lead & ( 0x7FU >> length );
		for ( std::size_t i = 1; i < length; ++i )
		{
			const auto c = static_cast<unsigned char>( text[pos + i] );
			if ( ( c & 0xC0U ) != 0x80U )
			{
				++pos;
				return replacement_character;
			}
			cp = ( cp << 6U ) | ( c & 0x3FU );
		}

		const bool overlong  = ( length == 3 && cp < 0x800 ) || ( length == 4 && cp < 0x10000 );
		const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
		if ( overlong || surrogate || cp > 0x10FFFF )
		{
			++pos;
			return replacement_character;
		}

		pos += length;
		return cp;
	}

	using EncodeBuffer = std::array<char, 4>;

	constexpr std::size_t encode( char32_t cp, EncodeBuffer& out ) noexcept
	{
		if ( cp >= 0xD800 && cp <= 0xDFFF )
		{
			cp = replacement_character;
		}
		if ( cp > 0x10FFFF )
		{
			cp = replacement_character;
		}

		if ( cp < 0x80 )
		{
			out[0] = static_cast<char>( cp );
			return 1;
		}
		if ( cp < 0x800 )
		{
			out[0] = static_cast<char>( 0xC0U | ( cp >> 6U ) );
			out[1] = static_cast<char>( 0x80U | ( cp & 0x3FU ) );
			return 2;
		}
		if ( cp < 0x10000 )
		{
			out[0] = static_cast<char>( 0xE0U | ( cp >> 12U ) );
			out[1] = static_cast<char>( 0x80U | ( ( cp >> 6U ) & 0x3FU ) );
			out[2] = static_cast<char>( 0x80U | ( cp & 0x3FU ) );
			return 3;
		}
		out[0] = static_cast<char>( 0xF0U | ( cp >> 18U ) );
		out[1] = static_cast<char>( 0x80U | ( ( cp >> 12U ) & 0x3FU ) );
		out[2] = static_cast<char>( 0x80U | ( ( cp >> 6U ) & 0x3FU ) );
		out[3] = static_cast<char>( 0x80U | ( cp & 0x3FU ) );
		return 4;
	}

	inline void append( std::string& out, char32_t cp )
	{
		EncodeBuffer buffer{};
		out.append( buffer.data(), encode( cp, buffer ) );
	}

	[[nodiscard]] constexpr std::size_t length( std::string_view text ) noexcept
	{
		std::size_t count = 0;
		for ( std::size_t pos = 0; pos < text.size(); )
		{
			( void )decode( text, pos );
			++count;
		}
		return count;
	}

	// Byte offset of code point number `codepoints`, clamped to the end of the text.
	[[nodiscard]] constexpr std::size_t offsetOf( std::string_view text, std::size_t codepoints ) noexcept
	{
		std::size_t pos = 0;
		for ( std::size_t i = 0; i < codepoints && pos < text.size(); ++i )
		{
			( void )decode( text, pos );
		}
		return pos;
	}

	[[nodiscard]] constexpr std::string_view prefix( std::string_view text, std::size_t codepoints ) noexcept
	{
		return text.substr( 0, offsetOf( text, codepoints ) );
	}

	[[nodiscard]] constexpr char32_t first( std::string_view text ) noexcept
	{
		if ( text.empty() )
		{
			return 0;
		}
		std::size_t pos = 0;
		return decode( text, pos );
	}

	[[nodiscard]] constexpr char32_t last( std::string_view text ) noexcept
	{
		if ( text.empty() )
		{
			return 0;
		}
		std::size_t start = text.size() - 1;
		while ( start > 0 && ( static_cast<unsigned char>( text[start] ) & 0xC0U ) == 0x80U && text.size() - start < 4 )
		{
			--start;
		}
		return decode( text, start );
	}

	[[nodiscard]] std::u32string toUtf32( std::string_view text );

	[[nodiscard]] std::string fromUtf32( std::u32string_view text );

	class CodepointRange
	{
	public:
		class iterator
		{
		public:
			using value_type        = char32_t;
			using difference_type   = std::ptrdiff_t;
			using iterator_category = std::forward_iterator_tag;

			constexpr iterator() noexcept = default;

			constexpr iterator( std::string_view text, std::size_t pos ) noexcept :
				text_( text ),
				pos_( pos )
			{
				load();
			}

			constexpr char32_t operator*() const noexcept
			{
				return current_;
			}

			[[nodiscard]] constexpr std::size_t offset() const noexcept
			{
				return pos_;
			}

			constexpr iterator& operator++() noexcept
			{
				pos_ = next_;
				load();
				return *this;
			}

			constexpr iterator operator++( int ) noexcept
			{
				iterator copy = *this;
				++*this;
				return copy;
			}

			constexpr bool operator==( const iterator& other ) const noexcept
			{
				return pos_ == other.pos_;
			}

		private:
			constexpr void load() noexcept
			{
				next_ = pos_;
				if ( pos_ < text_.size() )
				{
					current_ = decode( text_, next_ );
				}
			}

			std::string_view text_;
			std::size_t      pos_     = 0;
			std::size_t      next_    = 0;
			char32_t         current_ = 0;
		};

		constexpr explicit CodepointRange( std::string_view text ) noexcept :
			text_( text )
		{
		}

		[[nodiscard]] constexpr iterator begin() const noexcept
		{
			return { text_, 0 };
		}

		[[nodiscard]] constexpr iterator end() const noexcept
		{
			return { text_, text_.size() };
		}

	private:
		std::string_view text_;
	};

	[[nodiscard]] constexpr CodepointRange codepoints( std::string_view text ) noexcept
	{
		return CodepointRange( text );
	}

} // namespace lexiglance::utf8

#endif // LEXIGLANCE_CORE_UTF8_H
