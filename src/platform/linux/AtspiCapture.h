#ifndef LEXIGLANCE_PLATFORM_LINUX_ATSPICAPTURE_H
#define LEXIGLANCE_PLATFORM_LINUX_ATSPICAPTURE_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	// Text under the pointer through the accessibility bus (GTK, Qt, Firefox, Chromium, LibreOffice, ...).
	[[nodiscard]] std::unique_ptr<TextCapture> createAtspiCapture( bool enable_accessibility );

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_LINUX_ATSPICAPTURE_H
