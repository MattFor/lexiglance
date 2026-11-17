#ifndef LEXIGLANCE_PLATFORM_MACOS_CGSCREEN_H
#define LEXIGLANCE_PLATFORM_MACOS_CGSCREEN_H

#include "../ocr/OcrCapture.h"

namespace lexiglance::platform
{

	// Screen pixels for OCR through CoreGraphics, which needs the Screen Recording permission.
	[[nodiscard]] Result<std::unique_ptr<ScreenReader>> createCgScreen();

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_MACOS_CGSCREEN_H
