#ifndef LEXIGLANCE_PLATFORM_MACOS_AXCAPTURE_H
#define LEXIGLANCE_PLATFORM_MACOS_AXCAPTURE_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	// Text under the pointer through the macOS accessibility API (AXUIElement), which needs the Accessibility permission.
	[[nodiscard]] std::unique_ptr<TextCapture> createAxCapture();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_MACOS_AXCAPTURE_H
