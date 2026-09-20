#ifndef LEXIGLANCE_PLATFORM_PLATFORM_H
#define LEXIGLANCE_PLATFORM_PLATFORM_H

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Error.h>
#include <lexiglance/core/Health.h>
#include <lexiglance/render/Highlight.h>
#include <lexiglance/render/PopupRenderer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Desktop integration. Everything here is passive: input is observed, never grabbed, injected or synthesised; no
// other process is ever read, traced or modified. See docs/anticheat.md.
namespace lexiglance::platform
{

	struct Point
	{
		int x = 0;
		int y = 0;
	};

	struct Rect
	{
		int x      = 0;
		int y      = 0;
		int width  = 0;
		int height = 0;

		[[nodiscard]] bool contains( Point p ) const noexcept
		{
			return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
		}

		[[nodiscard]] bool empty() const noexcept
		{
			return width <= 0 || height <= 0;
		}

		// The smallest rectangle holding both.
		[[nodiscard]] Rect united( const Rect& other ) const noexcept
		{
			const int left   = std::min( x, other.x );
			const int top    = std::min( y, other.y );
			const int right  = std::max( x + width, other.x + other.width );
			const int bottom = std::max( y + height, other.y + other.height );
			return { .x = left, .y = top, .width = right - left, .height = bottom - top };
		}
	};

	struct WindowInfo
	{
		std::uint64_t id  = 0;
		std::uint32_t pid = 0;
		std::string   wm_class;
		bool          fullscreen = false;
		bool          own        = false;
	};

	class TextCapture;

	// How much a capture reads.
	struct CaptureScope
	{
		// Characters from the pointer on (Scanning -> the longest text looked up).
		std::size_t characters = 20;
		// The whole sentence around the pointer too, where it runs on over several lines: for a translation, or text chosen
		// with the wheel beyond what was read. OCR reads the other lines only then, since each one costs time.
		bool sentence = false;
	};

	struct CapturedText
	{
		std::string           text;
		std::int32_t          offset = 0;
		Rect                  character;
		std::shared_ptr<void> handle;
		// Surrounding text (sentence or line) and the byte offset of `text` within it, for Anki notes.
		std::string sentence;
		std::size_t sentence_offset = 0;
		// Characters at the start of `text` that come before the one under the pointer: the beginning of its word, put in
		// front from `sentence` for languages that separate words. bounds() counts from the first of them.
		std::size_t rewound = 0;
		// Whether the spaces between words are still in `text` and `sentence`. OCR that reads a line as Japanese, set on
		// a grid, drops them: moving back to the start of the word would then run into the words before it.
		bool spaces = true;
		// How sure the capture is of the text, 0 to 100 (accessibility text is exact; OCR reports its confidence).
		float confidence = 100.0F;
		// The capture that produced the text; it answers bounds() for it.
		TextCapture* origin = nullptr;
	};

	// Reads the text under the pointer. Created, used and destroyed on the capture thread only.
	class TextCapture
	{
	public:
		virtual ~TextCapture() = default;

		[[nodiscard]] virtual std::string_view name() const noexcept = 0;

		// State shown in the settings application, e.g. "at-spi + OCR (jpn, fast)". A summary: it sits in a small tile,
		// so the reason a part is missing belongs in problem() and the health report, not here.
		[[nodiscard]] virtual std::string describe() const
		{
			return std::string( name() );
		}

		// Why a part of this capture is not working, in full, or empty when everything it was asked for runs.
		[[nodiscard]] virtual std::string problem() const
		{
			return {};
		}

		[[nodiscard]] virtual std::optional<CapturedText> capture( Point point, const WindowInfo& window, CaptureScope scope ) = 0;

		// Screen bounds of the first `length` characters of a capture (for the highlight overlay).
		[[nodiscard]] virtual std::optional<Rect> bounds( const CapturedText& text, std::size_t length ) = 0;

		// The same, a rectangle for each line the characters are on, so a highlight over text that wraps covers just the
		// text. Captures that cannot tell lines apart give the one rectangle.
		[[nodiscard]] virtual std::vector<Rect> lineBounds( const CapturedText& text, std::size_t length )
		{
			const auto whole = bounds( text, length );
			return whole ? std::vector<Rect>{ *whole } : std::vector<Rect>{};
		}

		// Upkeep while no capture is asked for, e.g. taking in the accessibility bus's messages, which pile up otherwise.
		// Returns how soon it wants to run again (nullopt: never).
		virtual std::optional<std::chrono::milliseconds> idle()
		{
			return std::nullopt;
		}

		// Self-checks for the health report.
		virtual void diagnose( std::vector<health::Check>& /*out*/ ) {}
	};

	// Callbacks, all invoked on the UI thread.
	struct Events
	{
		// The pointer rests over text with the trigger held; `sentence`: the sentence key is held too, `sentence_pressed`:
		// it went down just now (a press turns a translation on and off again; holding it while the pointer moves does not).
		std::function<void( Point, const WindowInfo&, bool sentence, bool sentence_pressed )> scan;
		std::function<void()>                                                                 trigger_released;
		// The trigger chord became held (true) or was let go (false).
		std::function<void( bool )>               trigger_changed;
		std::function<void( Point )>              click_outside;
		std::function<void( std::string, Point )> selection;
		// A button in the popup was clicked: the entry's index in the shown result.
		std::function<void( std::size_t, render::PopupAction )> popup_action;
		// The wheel turned while the trigger is held: +1 lengthens the looked-up text, -1 shortens it.
		std::function<void( int )> adjust_length;
		// The window the popup's text came from was closed, minimised or moved to another workspace.
		std::function<void()> source_closed;
	};

	struct PopupContent
	{
		std::shared_ptr<const render::PopupImage> image;
		render::PopupStyle                        style;
		Rect                                      anchor;
		// The window the text came from (0 if none); the popup closes with it.
		std::uint64_t source = 0;
	};

	class Backend
	{
	public:
		virtual ~Backend() = default;

		[[nodiscard]] virtual std::string_view name() const noexcept = 0;

		virtual Result<> start( Events events ) = 0;

		// Runs the UI event loop until quit().
		virtual void run() = 0;

		// Thread-safe.
		virtual void quit() = 0;

		// Thread-safe; the task runs on the UI thread.
		virtual void post( std::move_only_function<void()> task ) = 0;

		// UI thread only from here on.
		virtual void               configure( const config::Config& config ) = 0;
		virtual void               showPopup( PopupContent content )         = 0;
		virtual void               hidePopup()                               = 0;
		[[nodiscard]] virtual bool popupVisible() const                      = 0;
		// Marks text on screen: a rectangle for each line of it.
		virtual void showHighlight( std::span<const Rect> rects, const render::Color& color ) = 0;
		virtual void hideHighlight()                                                          = 0;
		// A short status in the corner of the popup, e.g. "Copied ✓".
		virtual void                     showBadge( std::string text, std::chrono::milliseconds duration ) = 0;
		[[nodiscard]] virtual Point      pointer()                                                         = 0;
		[[nodiscard]] virtual WindowInfo windowAt( Point point )                                           = 0;

		// Reports the keys and buttons held together once all of them are released (for trigger configuration).
		virtual void recordChord( std::function<void( std::vector<std::string> )> done ) = 0;
		virtual void cancelRecording()                                                   = 0;

		// Self-checks of the desktop integration (trigger keys, keyboard state) for the health report. `interactive`: the
		// user just clicked to start the check, so input events must have arrived.
		virtual void diagnose( std::vector<health::Check>& out, bool interactive ) = 0;

		// Thread-safe snapshots of desktop settings.
		[[nodiscard]] virtual double scaleFactor() const          = 0;
		[[nodiscard]] virtual bool   prefersDarkTheme() const     = 0;
		[[nodiscard]] virtual bool   supportsTransparency() const = 0;

		// When the last keyboard or mouse event arrived from the desktop (thread-safe); the epoch when none did yet.
		[[nodiscard]] virtual std::chrono::steady_clock::time_point lastInput() const = 0;

		// Called on the capture thread; may return nullptr when no capture method is available.
		[[nodiscard]] virtual std::unique_ptr<TextCapture> createTextCapture( const config::Config& config ) = 0;
	};

	[[nodiscard]] Result<std::unique_ptr<Backend>> createBackend();

	// The highlight the settings ask for.
	[[nodiscard]] render::HighlightLook lookFor( const config::PopupSettings& popup ) noexcept;

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_PLATFORM_H
