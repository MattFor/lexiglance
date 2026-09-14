#ifndef LEXIGLANCE_LANGUAGE_CASE_H
#define LEXIGLANCE_LANGUAGE_CASE_H

// Upper and lower case of the Latin (Basic, Latin-1, Extended-A), Greek and Cyrillic letters, which covers the
// alphabets of most languages written with case. Other characters are returned unchanged.
namespace lexiglance::lang
{

	namespace detail
	{

		// Blocks where an upper case letter is followed by its lower case one.
		[[nodiscard]] constexpr bool pairedUpper( char32_t c ) noexcept
		{
			const bool even = c % 2 == 0;
			return ( c >= 0x100 && c <= 0x12F && even ) || ( c >= 0x132 && c <= 0x137 && even ) || ( c >= 0x139 && c <= 0x148 && !even ) || ( c >= 0x14A && c <= 0x177 && even ) || ( c >= 0x179 && c <= 0x17E && !even ) ||
			       ( c >= 0x460 && c <= 0x481 && even ) || ( c >= 0x48A && c <= 0x4BF && even ) || ( c >= 0x4C1 && c <= 0x4CE && !even ) || ( c >= 0x4D0 && c <= 0x52F && even );
		}

	} // namespace detail

	[[nodiscard]] constexpr char32_t lowercase( char32_t c ) noexcept
	{
		if ( ( c >= 'A' && c <= 'Z' ) || ( c >= 0xC0 && c <= 0xDE && c != 0xD7 ) || ( c >= 0x391 && c <= 0x3AB && c != 0x3A2 ) || ( c >= 0x410 && c <= 0x42F ) )
		{
			return c + 0x20;
		}
		if ( c >= 0x400 && c <= 0x40F )
		{
			return c + 0x50;
		}
		if ( detail::pairedUpper( c ) )
		{
			return c + 1;
		}
		switch ( c )
		{
			case 0x130: // İ
				return 'i';
			case 0x178: // Ÿ
				return 0xFF;
			case 0x386:
				return 0x3AC;
			case 0x388:
			case 0x389:
			case 0x38A:
				return c + 0x25;
			case 0x38C:
				return 0x3CC;
			case 0x38E:
			case 0x38F:
				return c + 0x3F;
			case 0x4C0: // Ӏ
				return 0x4CF;
			default:
				return c;
		}
	}

	[[nodiscard]] constexpr char32_t uppercase( char32_t c ) noexcept
	{
		if ( ( c >= 'a' && c <= 'z' ) || ( c >= 0xE0 && c <= 0xFE && c != 0xF7 ) || ( c >= 0x3B1 && c <= 0x3CB && c != 0x3C2 ) || ( c >= 0x430 && c <= 0x44F ) )
		{
			return c - 0x20;
		}
		if ( c >= 0x450 && c <= 0x45F )
		{
			return c - 0x50;
		}
		if ( c > 0 && detail::pairedUpper( c - 1 ) )
		{
			return c - 1;
		}
		switch ( c )
		{
			case 0x131: // ı
				return 'I';
			case 0xFF: // ÿ
				return 0x178;
			case 0x3C2: // final ς
				return 0x3A3;
			case 0x3AC:
				return 0x386;
			case 0x3AD:
			case 0x3AE:
			case 0x3AF:
				return c - 0x25;
			case 0x3CC:
				return 0x38C;
			case 0x3CD:
			case 0x3CE:
				return c - 0x3F;
			case 0x4CF:
				return 0x4C0;
			default:
				return c;
		}
	}

	[[nodiscard]] constexpr bool isUppercase( char32_t c ) noexcept
	{
		return lowercase( c ) != c;
	}

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_CASE_H
