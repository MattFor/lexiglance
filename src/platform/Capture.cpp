#include "Capture.h"

#include <lexiglance/core/Glob.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/ocr/Onnx.h>

#include <algorithm>

namespace lexiglance::platform
{

	ChainCapture::ChainCapture( std::unique_ptr<TextCapture> accessibility, std::unique_ptr<TextCapture> ocr, config::OcrMode mode, std::vector<std::string> ocr_windows, std::string ocr_state ) :
		accessibility_( std::move( accessibility ) ),
		ocr_( std::move( ocr ) ),
		mode_( mode ),
		ocr_windows_( std::move( ocr_windows ) ),
		ocr_state_( std::move( ocr_state ) )
	{
	}

	std::string ChainCapture::describe() const
	{
		std::string out = accessibility_ ? accessibility_->describe() : std::string( "no accessibility" );
		out.append( " + " );
		if ( ocr_ )
		{
			out.append( ocr_->describe() );
		}
		else if ( mode_ == config::OcrMode::Off )
		{
			out.append( "OCR off" );
		}
		else
		{
			// Why is in problem() and the health report; the settings application points at it from here.
			out.append( "OCR unavailable" );
		}
		return out;
	}

	std::string ChainCapture::problem() const
	{
		return mode_ != config::OcrMode::Off && !ocr_ ? ocr_state_ : std::string();
	}

	bool ChainCapture::prefersOcr( const WindowInfo& window ) const
	{
		return mode_ == config::OcrMode::Always || std::ranges::any_of( ocr_windows_, [&]( const std::string& pattern ) { return globMatch( pattern, window.wm_class ); } );
	}

	std::optional<CapturedText> ChainCapture::capture( Point point, const WindowInfo& window, CaptureScope scope )
	{
		const bool ocr_enabled = ocr_ && mode_ != config::OcrMode::Off;
		const bool ocr_first   = ocr_enabled && prefersOcr( window );

		const auto attempt = [&]( TextCapture* source ) -> std::optional<CapturedText> {
			if ( source == nullptr )
			{
				return std::nullopt;
			}
			auto text = source->capture( point, window, scope );
			if ( text && !text->text.empty() )
			{
				text->origin = source;
				return text;
			}
			return std::nullopt;
		};

		TextCapture* const first  = ocr_first ? ocr_.get() : accessibility_.get();
		TextCapture*       second = ocr_first ? accessibility_.get() : nullptr;
		if ( !ocr_first && ocr_enabled )
		{
			second = ocr_.get();
		}
		if ( auto text = attempt( first ) )
		{
			// Accessibility often returns English chrome beside an image (a README figure, a game HUD label). When that
			// text is not in a language Lexiglance reads, try OCR so the pixels under the pointer still get a chance.
			if ( first == accessibility_.get() && second != nullptr && !lang::startsInKnownScript( text->text ) )
			{
				if ( auto ocr = attempt( second ) )
				{
					return ocr;
				}
			}
			return text;
		}
		return attempt( second );
	}

	std::optional<Rect> ChainCapture::bounds( const CapturedText& text, std::size_t length )
	{
		if ( text.origin == nullptr || text.origin == this )
		{
			return std::nullopt;
		}
		return text.origin->bounds( text, length );
	}

	std::vector<Rect> ChainCapture::lineBounds( const CapturedText& text, std::size_t length )
	{
		if ( text.origin == nullptr || text.origin == this )
		{
			return {};
		}
		return text.origin->lineBounds( text, length );
	}

	std::optional<std::chrono::milliseconds> ChainCapture::idle()
	{
		std::optional<std::chrono::milliseconds> next;
		for ( TextCapture* source : { accessibility_.get(), ocr_.get() } )
		{
			if ( source == nullptr )
			{
				continue;
			}
			if ( const auto wanted = source->idle(); wanted && ( !next || *wanted < *next ) )
			{
				next = wanted;
			}
		}
		return next;
	}

	void ChainCapture::diagnose( std::vector<health::Check>& out )
	{
		if ( accessibility_ )
		{
			accessibility_->diagnose( out );
		}
		else
		{
			out.push_back(
					{ .id     = "accessibility",
			          .title  = "Text in applications",
			          .status = health::Severity::Warning,
			          .detail = "This build cannot read text through the accessibility bus; only OCR and selections work." }
			);
		}

		if ( mode_ == config::OcrMode::Off )
		{
			out.push_back(
					{ .id     = "ocr",
			          .title  = "Text in images, games and videos (OCR)",
			          .status = health::Severity::Warning,
			          .detail = "OCR is turned off, so text that applications do not expose (images, games, videos, Discord, Wine) is not read.",
			          .fix    = "enable-ocr" }
			);
		}
		else if ( !ocr_ )
		{
			// A Windows without Microsoft's runtime is the one reason the settings application can put right by
			// itself; everything else is a download to choose on the Scanning page.
			const bool runtime = ocr_state_.contains( ocr::vc_runtime_absent );
			out.push_back(
					{ .id     = "ocr",
			          .title  = "Text in images, games and videos (OCR)",
			          .status = health::Severity::Error,
			          .detail = ocr_state_,
			          .fix    = runtime ? "install-vcredist" : "open-scanning" }
			);
		}
		else
		{
			ocr_->diagnose( out );
		}
	}

} // namespace lexiglance::platform
