#ifndef LEXIGLANCE_PLATFORM_CAPTURE_H
#define LEXIGLANCE_PLATFORM_CAPTURE_H

#include <lexiglance/platform/Platform.h>

#include <memory>
#include <string>
#include <vector>

namespace lexiglance::platform
{

	// Combines the accessibility reader with screen text recognition: accessibility first (exact text), OCR for
	// everything that exposes no text. Windows matching `ocr_windows` (games) go to OCR first.
	class ChainCapture final : public TextCapture
	{
	public:
		ChainCapture( std::unique_ptr<TextCapture> accessibility, std::unique_ptr<TextCapture> ocr, config::OcrMode mode, std::vector<std::string> ocr_windows, std::string ocr_state );

		[[nodiscard]] std::string_view name() const noexcept override
		{
			return "chain";
		}

		[[nodiscard]] std::string describe() const override;

		[[nodiscard]] std::optional<CapturedText> capture( Point point, const WindowInfo& window, std::size_t max_chars ) override;

		[[nodiscard]] std::optional<Rect> bounds( const CapturedText& text, std::size_t length ) override;

		std::optional<std::chrono::milliseconds> idle() override;

		void diagnose( std::vector<health::Check>& out ) override;

	private:
		[[nodiscard]] bool prefersOcr( const WindowInfo& window ) const;

		std::unique_ptr<TextCapture> accessibility_;
		std::unique_ptr<TextCapture> ocr_;
		config::OcrMode              mode_;
		std::vector<std::string>     ocr_windows_;
		std::string                  ocr_state_;
	};

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_CAPTURE_H
