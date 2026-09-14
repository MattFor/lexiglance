#ifndef LEXIGLANCE_PLATFORM_WINDOWS_WINDOWSBACKEND_H
#define LEXIGLANCE_PLATFORM_WINDOWS_WINDOWSBACKEND_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	[[nodiscard]] Result<std::unique_ptr<Backend>> createWindowsBackend();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_WINDOWS_WINDOWSBACKEND_H
