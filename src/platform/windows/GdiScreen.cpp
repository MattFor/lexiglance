#include "GdiScreen.h"

#include <algorithm>
#include <cstdint>

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <dwmapi.h>

namespace lexiglance::platform
{

	namespace
	{

		class GdiScreen final : public ScreenReader
		{
		public:
			GdiScreen( HDC screen, HDC memory ) :
				screen_( screen ),
				memory_( memory )
			{
			}

			~GdiScreen() override
			{
				release();
				DeleteDC( memory_ );
				ReleaseDC( nullptr, screen_ );
			}

			GdiScreen( const GdiScreen& )            = delete;
			GdiScreen& operator=( const GdiScreen& ) = delete;
			GdiScreen( GdiScreen&& )                 = delete;
			GdiScreen& operator=( GdiScreen&& )      = delete;

			Rect bounds() override
			{
				return {
					.x      = GetSystemMetrics( SM_XVIRTUALSCREEN ),
					.y      = GetSystemMetrics( SM_YVIRTUALSCREEN ),
					.width  = GetSystemMetrics( SM_CXVIRTUALSCREEN ),
					.height = GetSystemMetrics( SM_CYVIRTUALSCREEN ),
				};
			}

			std::optional<Rect> windowRect( std::uint64_t id ) override
			{
				auto* const window = reinterpret_cast<HWND>( static_cast<std::uintptr_t>( id ) );
				RECT        rect{};
				if ( IsWindow( window ) == FALSE )
				{
					return std::nullopt;
				}
				// The visible frame: GetWindowRect also counts the invisible resize borders around it.
				if ( FAILED( DwmGetWindowAttribute( window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof( rect ) ) ) && GetWindowRect( window, &rect ) == FALSE )
				{
					return std::nullopt;
				}
				return Rect{ .x = rect.left, .y = rect.top, .width = rect.right - rect.left, .height = rect.bottom - rect.top };
			}

			std::vector<std::uint8_t> read( const Rect& region ) override
			{
				if ( region.empty() || !reserve( region.width, region.height ) )
				{
					return {};
				}
				// The secure desktop (a UAC prompt, the lock screen) cannot be read; BitBlt fails there.
				if ( BitBlt( memory_, 0, 0, region.width, region.height, screen_, region.x, region.y, SRCCOPY | CAPTUREBLT ) == FALSE )
				{
					return {};
				}
				GdiFlush();

				std::vector<std::uint8_t> rgb( static_cast<std::size_t>( region.width ) * static_cast<std::size_t>( region.height ) * 3 );
				const auto                stride = static_cast<std::size_t>( width_ ) * 4;
				for ( int y = 0; y < region.height; ++y )
				{
					const std::uint8_t* row = bits_ + ( static_cast<std::size_t>( y ) * stride );
					std::uint8_t*       out = rgb.data() + ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( region.width ) * 3 );
					for ( int x = 0; x < region.width; ++x )
					{
						// BGRX in the DIB.
						out[0] = row[2];
						out[1] = row[1];
						out[2] = row[0];
						row += 4;
						out += 3;
					}
				}
				return rgb;
			}

		private:
			// A top-down 32-bit DIB at least width x height, kept between reads.
			bool reserve( int width, int height )
			{
				if ( bitmap_ != nullptr && width <= width_ && height <= height_ )
				{
					return true;
				}
				release();
				width  = std::max( width, width_ );
				height = std::max( height, height_ );

				BITMAPINFO info{};
				info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
				info.bmiHeader.biWidth       = width;
				info.bmiHeader.biHeight      = -height;
				info.bmiHeader.biPlanes      = 1;
				info.bmiHeader.biBitCount    = 32;
				info.bmiHeader.biCompression = BI_RGB;
				void* bits                   = nullptr;
				bitmap_                      = CreateDIBSection( screen_, &info, DIB_RGB_COLORS, &bits, nullptr, 0 );
				if ( bitmap_ == nullptr || bits == nullptr )
				{
					bitmap_ = nullptr;
					return false;
				}
				previous_ = SelectObject( memory_, bitmap_ );
				bits_     = static_cast<const std::uint8_t*>( bits );
				width_    = width;
				height_   = height;
				return true;
			}

			void release() noexcept
			{
				if ( bitmap_ != nullptr )
				{
					SelectObject( memory_, previous_ );
					DeleteObject( bitmap_ );
					bitmap_ = nullptr;
					bits_   = nullptr;
				}
			}

			HDC                 screen_;
			HDC                 memory_;
			HBITMAP             bitmap_   = nullptr;
			HGDIOBJ             previous_ = nullptr;
			const std::uint8_t* bits_     = nullptr;
			int                 width_    = 0;
			int                 height_   = 0;
		};

	} // namespace

	Result<std::unique_ptr<ScreenReader>> createGdiScreen()
	{
		HDC screen = GetDC( nullptr );
		if ( screen == nullptr )
		{
			return fail( "cannot read the screen (GetDC failed)" );
		}
		HDC memory = CreateCompatibleDC( screen );
		if ( memory == nullptr )
		{
			ReleaseDC( nullptr, screen );
			return fail( "cannot read the screen (CreateCompatibleDC failed)" );
		}
		return std::make_unique<GdiScreen>( screen, memory );
	}

} // namespace lexiglance::platform
