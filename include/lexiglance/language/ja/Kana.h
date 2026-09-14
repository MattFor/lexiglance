#ifndef LEXIGLANCE_LANGUAGE_JA_KANA_H
#define LEXIGLANCE_LANGUAGE_JA_KANA_H

#include <string>
#include <string_view>

namespace lexiglance::lang::ja
{

	[[nodiscard]] constexpr bool isHiragana( char32_t c ) noexcept
	{
		return ( c >= 0x3041 && c <= 0x3096 ) || ( c >= 0x309D && c <= 0x309F );
	}

	[[nodiscard]] constexpr bool isKatakana( char32_t c ) noexcept
	{
		return ( c >= 0x30A1 && c <= 0x30FA ) || ( c >= 0x30FC && c <= 0x30FF ) || ( c >= 0x31F0 && c <= 0x31FF ) || ( c >= 0xFF66 && c <= 0xFF9F );
	}

	[[nodiscard]] constexpr bool isKana( char32_t c ) noexcept
	{
		return isHiragana( c ) || isKatakana( c );
	}

	[[nodiscard]] constexpr bool isKanji( char32_t c ) noexcept
	{
		return ( c >= 0x4E00 && c <= 0x9FFF ) || ( c >= 0x3400 && c <= 0x4DBF ) || ( c >= 0xF900 && c <= 0xFAFF ) || ( c >= 0x20000 && c <= 0x3134F ) || c == 0x3005 ||
		       c == 0x3006 || c == 0x3007 || c == 0x30F6;
	}

	[[nodiscard]] constexpr bool isFullwidthAlphanumeric( char32_t c ) noexcept
	{
		return ( c >= 0xFF10 && c <= 0xFF19 ) || ( c >= 0xFF21 && c <= 0xFF3A ) || ( c >= 0xFF41 && c <= 0xFF5A );
	}

	[[nodiscard]] constexpr bool isAsciiAlphanumeric( char32_t c ) noexcept
	{
		return ( c >= '0' && c <= '9' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' );
	}

	[[nodiscard]] constexpr char32_t toHiragana( char32_t c ) noexcept
	{
		if ( c >= 0x30A1 && c <= 0x30F6 )
		{
			return c - 0x60;
		}
		if ( c == 0x30FD || c == 0x30FE )
		{
			return c - 0x60;
		}
		return c;
	}

	[[nodiscard]] constexpr char32_t toKatakana( char32_t c ) noexcept
	{
		if ( c >= 0x3041 && c <= 0x3096 )
		{
			return c + 0x60;
		}
		if ( c == 0x309D || c == 0x309E )
		{
			return c + 0x60;
		}
		return c;
	}

	// Combines kana with a following (semi-)voiced sound mark, returning 0 when they do not combine.
	[[nodiscard]] constexpr char32_t combineMark( char32_t c, bool semi_voiced ) noexcept
	{
		const bool     katakana = c >= 0x30A1 && c <= 0x30F6;
		const char32_t h        = katakana ? c - 0x60 : c;
		const bool     ha_row   = h >= 0x306F && h <= 0x307B && ( h - 0x306F ) % 3 == 0;

		if ( semi_voiced )
		{
			return ha_row ? c + 2 : 0;
		}
		if ( h == 0x3046 )
		{
			return katakana ? 0x30F4 : 0x3094;
		}
		const bool ka_to_chi = h >= 0x304B && h <= 0x3061 && ( h - 0x304B ) % 2 == 0;
		const bool tsu_te_to = h == 0x3064 || h == 0x3066 || h == 0x3068;
		return ka_to_chi || tsu_te_to || ha_row ? c + 1 : 0;
	}

	[[nodiscard]] std::string toHiragana( std::string_view text );

	[[nodiscard]] std::string toKatakana( std::string_view text );

} // namespace lexiglance::lang::ja

#endif // LEXIGLANCE_LANGUAGE_JA_KANA_H
