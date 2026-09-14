#ifndef LEXIGLANCE_PLATFORM_WINDOWS_UIACAPTURE_H
#define LEXIGLANCE_PLATFORM_WINDOWS_UIACAPTURE_H

#include <lexiglance/platform/Platform.h>

namespace lexiglance::platform
{

	// Text under the pointer through UI Automation's text pattern (Win32 edit controls, WPF, WinUI, Qt, Chromium,
	// Firefox, Office, consoles). Created on the capture thread, which it joins to COM's multithreaded apartment.
	[[nodiscard]] std::unique_ptr<TextCapture> createUiaCapture();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_WINDOWS_UIACAPTURE_H
