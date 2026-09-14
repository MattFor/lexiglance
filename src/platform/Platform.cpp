#include <lexiglance/platform/Platform.h>

#ifdef LEXIGLANCE_PLATFORM_X11
	#include "linux/X11Backend.h"
#endif
#ifdef LEXIGLANCE_PLATFORM_WINDOWS
	#include "windows/WindowsBackend.h"
#endif

namespace lexiglance::platform
{

	Result<std::unique_ptr<Backend>> createBackend()
	{
#ifdef LEXIGLANCE_PLATFORM_X11
		return createX11Backend();
#elifdef LEXIGLANCE_PLATFORM_WINDOWS
		return createWindowsBackend();
#else
		return fail( "this platform has no desktop integration yet (see docs/porting.md)" );
#endif
	}

	render::HighlightLook lookFor( const config::PopupSettings& popup ) noexcept
	{
		render::HighlightShape shape = render::HighlightShape::Underline;
		switch ( popup.highlight_style )
		{
			case config::HighlightStyle::Underline:
				shape = render::HighlightShape::Underline;
				break;
			case config::HighlightStyle::Outline:
				shape = render::HighlightShape::Outline;
				break;
			case config::HighlightStyle::Fill:
				shape = render::HighlightShape::Fill;
				break;
			case config::HighlightStyle::DoubleUnderline:
				shape = render::HighlightShape::DoubleUnderline;
				break;
			case config::HighlightStyle::DottedUnderline:
				shape = render::HighlightShape::DottedUnderline;
				break;
			case config::HighlightStyle::WavyUnderline:
				shape = render::HighlightShape::WavyUnderline;
				break;
			case config::HighlightStyle::Brackets:
				shape = render::HighlightShape::Brackets;
				break;
		}
		return { .shape = shape, .thickness = popup.highlight_thickness, .radius = popup.highlight_radius, .padding = popup.highlight_padding };
	}

} // namespace lexiglance::platform
