#ifndef LEXIGLANCE_PLATFORM_MACOS_MACBACKEND_H
#define LEXIGLANCE_PLATFORM_MACOS_MACBACKEND_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	[[nodiscard]] Result<std::unique_ptr<Backend>> createMacBackend();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_MACOS_MACBACKEND_H
