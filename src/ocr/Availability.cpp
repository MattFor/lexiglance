#include <lexiglance/ocr/Onnx.h>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
	#include <windows.h>
#endif

namespace lexiglance::ocr
{

	bool loadFailureActionable( const std::string_view message )
	{
		return !message.empty() && message != paddle_absent && message != runtime_absent;
	}

	std::string missingLibraries( const std::span<const char* const> names, const std::function<bool( const char* )>& present )
	{
		std::string missing;
		for ( const char* name : names )
		{
			if ( !present( name ) )
			{
				missing.append( missing.empty() ? "" : ", " ).append( name );
			}
		}
		return missing;
	}

	std::string missingVcRuntime()
	{
#ifdef _WIN32
		constexpr std::array names{ "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140.dll", "MSVCP140_1.dll" };
		// Already loaded ones answer from the process, the rest are searched for as onnxruntime.dll would.
		return missingLibraries( names, []( const char* name ) {
			const HMODULE library = LoadLibraryA( name );
			if ( library != nullptr )
			{
				FreeLibrary( library );
			}
			return library != nullptr;
		} );
#else
		return {};
#endif
	}

} // namespace lexiglance::ocr
