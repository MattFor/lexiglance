#ifndef LEXIGLANCE_PLATFORM_OCR_OCRCAPTURE_H
#define LEXIGLANCE_PLATFORM_OCR_OCRCAPTURE_H

#include <lexiglance/language/Language.h>
#include <lexiglance/platform/Platform.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lexiglance::platform
{

	// Whether screen pixel (x, y) is one of the dots of a stippled highlight (every `stipple`-th pixel: 2 or 4).
	[[nodiscard]] inline bool stippled( int x, int y, int stipple ) noexcept
	{
		const bool odd_x = ( x & 1 ) != 0;
		const bool odd_y = ( y & 1 ) != 0;
		return stipple == 4 ? !odd_x && !odd_y : odd_x == odd_y;
	}

	// One of our own windows on screen: the pixels it hides (grayscale, row by row) when they are known, or the
	// stipple of a dotted highlight, whose dots are repaired from their neighbours.
	struct Overlay
	{
		Rect                                             rect;
		std::shared_ptr<const std::vector<std::uint8_t>> underneath;
		int                                              stipple = 0;
		// Set for an area one of our windows has just left: until the application below has painted it again, it is not
		// compared with earlier reads (its pixels are read as they are).
		std::chrono::steady_clock::time_point until;
	};

	// The screen as OCR reads it, from the desktop backend (XGetImage on X11, BitBlt on Windows). Created for and used on
	// the capture thread only.
	class ScreenReader
	{
	public:
		virtual ~ScreenReader() = default;

		// The whole desktop in the coordinates of pointer positions (on Windows it can start left of or above 0,0).
		[[nodiscard]] virtual Rect bounds() = 0;

		// A top-level window's area on screen; nullopt when it is gone.
		[[nodiscard]] virtual std::optional<Rect> windowRect( std::uint64_t id ) = 0;

		// The pixels of a region within bounds(), RGB row by row; empty when they cannot be read.
		[[nodiscard]] virtual std::vector<std::uint8_t> read( const Rect& region ) = 0;
	};

	struct OcrOptions
	{
		config::OcrEngine engine   = config::OcrEngine::Auto;
		bool              vertical = false;
		std::string       model    = "fast";
		double            scale    = 1.0;
		// The languages to read (empty: every one); only their recognisers are loaded and run.
		std::vector<const lang::Language*> languages;
		// Thread-safe: screen areas currently covered by our own windows, which must not be read as text.
		std::function<std::vector<Overlay>()> overlays;
		// Opens the screen, once an OCR engine is loaded.
		std::function<Result<std::unique_ptr<ScreenReader>>()> screen;
	};

	// Reads text from the pixels around the pointer, for everything that exposes no text: games, images, video,
	// Wine, terminals without accessibility. It only reads the screen like any screenshot tool does.
	[[nodiscard]] Result<std::unique_ptr<TextCapture>> createOcrCapture( const OcrOptions& options );

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_OCR_OCRCAPTURE_H
