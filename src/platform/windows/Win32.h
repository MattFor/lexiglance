#ifndef LEXIGLANCE_PLATFORM_WINDOWS_WIN32_H
#define LEXIGLANCE_PLATFORM_WINDOWS_WIN32_H

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <string_view>

// Conversions between the UTF-8 used everywhere else and the UTF-16 of the Windows API.
namespace lexiglance::platform::win32
{

	[[nodiscard]] inline std::string narrow( std::wstring_view text )
	{
		if ( text.empty() )
		{
			return {};
		}
		const int   size = WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0, nullptr, nullptr );
		std::string out( static_cast<std::size_t>( std::max( 0, size ) ), '\0' );
		WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), size, nullptr, nullptr );
		return out;
	}

	[[nodiscard]] inline std::wstring wide( std::string_view text )
	{
		if ( text.empty() )
		{
			return {};
		}
		const int    size = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
		std::wstring out( static_cast<std::size_t>( std::max( 0, size ) ), L'\0' );
		MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), size );
		return out;
	}

} // namespace lexiglance::platform::win32

#endif // LEXIGLANCE_PLATFORM_WINDOWS_WIN32_H
