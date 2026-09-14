#ifndef LEXIGLANCE_PLATFORM_WINDOWS_GDISCREEN_H
#define LEXIGLANCE_PLATFORM_WINDOWS_GDISCREEN_H

#include "../ocr/OcrCapture.h"

namespace lexiglance::platform
{

	// Screen pixels for OCR: a BitBlt of the virtual screen, which is what the desktop compositor shows (layered windows
	// included), like any screenshot tool reads it.
	[[nodiscard]] Result<std::unique_ptr<ScreenReader>> createGdiScreen();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_WINDOWS_GDISCREEN_H
