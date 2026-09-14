#include "Capture.h"

#include <lexiglance/core/Glob.h>

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
		out.append( ocr_ ? ocr_->describe() : ocr_state_ );
		return out;
	}

	bool ChainCapture::prefersOcr( const WindowInfo& window ) const
	{
		return mode_ == config::OcrMode::Always || std::ranges::any_of( ocr_windows_, [&]( const std::string& pattern ) { return globMatch( pattern, window.wm_class ); } );
	}

	std::optional<CapturedText> ChainCapture::capture( Point point, const WindowInfo& window, std::size_t max_chars )
	{
		const bool ocr_enabled = ocr_ && mode_ != config::OcrMode::Off;
		const bool ocr_first   = ocr_enabled && prefersOcr( window );

		const auto attempt = [&]( TextCapture* source ) -> std::optional<CapturedText> {
			if ( source == nullptr )
			{
				return std::nullopt;
			}
			auto text = source->capture( point, window, max_chars );
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
			out.push_back(
					{ .id = "ocr", .title = "Text in images, games and videos (OCR)", .status = health::Severity::Error, .detail = ocr_state_, .fix = "open-scanning" }
			);
		}
		else
		{
			ocr_->diagnose( out );
		}
	}

} // namespace lexiglance::platform
