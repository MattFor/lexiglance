#ifndef LEXIGLANCE_PLATFORM_OCR_TESSERACT_H
#define LEXIGLANCE_PLATFORM_OCR_TESSERACT_H

#include <lexiglance/core/Error.h>
#include <lexiglance/platform/Platform.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::platform
{

	struct OcrSymbol
	{
		std::string text;
		Rect        box;
		int         line       = 0;
		float       confidence = 0.0F;
	};

	// libtesseract, loaded at runtime through its C API: no build dependency, and OCR is simply unavailable when the
	// library is not installed.
	class TesseractEngine
	{
	public:
		[[nodiscard]] static Result<std::unique_ptr<TesseractEngine>> load( const std::filesystem::path& datapath, const std::string& language, bool vertical );

		TesseractEngine( const TesseractEngine& )            = delete;
		TesseractEngine& operator=( const TesseractEngine& ) = delete;
		TesseractEngine( TesseractEngine&& )                 = delete;
		TesseractEngine& operator=( TesseractEngine&& )      = delete;
		~TesseractEngine();

		// Recognises an 8-bit grayscale image (dark text on a light background); boxes are in image pixels.
		[[nodiscard]] std::vector<OcrSymbol> recognize( std::span<const std::uint8_t> pixels, int width, int height );

		[[nodiscard]] std::string_view version() const noexcept
		{
			return version_;
		}

		struct Functions;

	private:
		TesseractEngine() = default;

		std::unique_ptr<Functions> functions_;
		void*                      library_ = nullptr;
		void*                      handle_  = nullptr;
		std::string                version_;
	};

	// Directory holding `<language>.traineddata`: Lexiglance's own model directory first, then the system tessdata.
	[[nodiscard]] std::optional<std::filesystem::path> findTessdata( std::string_view language, std::string_view model );

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_OCR_TESSERACT_H
