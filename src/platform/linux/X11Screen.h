#ifndef LEXIGLANCE_PLATFORM_LINUX_X11SCREEN_H
#define LEXIGLANCE_PLATFORM_LINUX_X11SCREEN_H

#include "../ocr/OcrCapture.h"

namespace lexiglance::platform
{

	// The root window's pixels for OCR, through a connection of its own: the capture thread never touches the UI
	// thread's display.
	[[nodiscard]] Result<std::unique_ptr<ScreenReader>> createX11Screen();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_LINUX_X11SCREEN_H
