#ifndef LEXIGLANCE_PLATFORM_LINUX_X11BACKEND_H
#define LEXIGLANCE_PLATFORM_LINUX_X11BACKEND_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	[[nodiscard]] Result<std::unique_ptr<Backend>> createX11Backend();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_LINUX_X11BACKEND_H
