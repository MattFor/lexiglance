#include "X11Screen.h"

#include <bit>
#include <cstring>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace lexiglance::platform
{

	namespace
	{

		int shiftOf( unsigned long mask )
		{
			return mask == 0 ? 0 : std::countr_zero( mask );
		}

		class X11Screen final : public ScreenReader
		{
		public:
			explicit X11Screen( Display* display ) :
				display_( display ),
				root_( DefaultRootWindow( display ) )
			{
			}

			~X11Screen() override
			{
				XCloseDisplay( display_ );
			}

			X11Screen( const X11Screen& )            = delete;
			X11Screen& operator=( const X11Screen& ) = delete;
			X11Screen( X11Screen&& )                 = delete;
			X11Screen& operator=( X11Screen&& )      = delete;

			Rect bounds() override
			{
				XWindowAttributes root{};
				XGetWindowAttributes( display_, root_, &root );
				return { .x = 0, .y = 0, .width = root.width, .height = root.height };
			}

			std::optional<Rect> windowRect( std::uint64_t id ) override
			{
				XWindowAttributes attributes{};
				if ( XGetWindowAttributes( display_, static_cast<Window>( id ), &attributes ) == 0 )
				{
					return std::nullopt;
				}
				int    x     = 0;
				int    y     = 0;
				Window child = 0;
				XTranslateCoordinates( display_, static_cast<Window>( id ), root_, 0, 0, &x, &y, &child );
				return Rect{ .x = x, .y = y, .width = attributes.width, .height = attributes.height };
			}

			std::vector<std::uint8_t> read( const Rect& region ) override
			{
				XImage* image = XGetImage( display_, root_, region.x, region.y, static_cast<unsigned>( region.width ), static_cast<unsigned>( region.height ), AllPlanes, ZPixmap );
				if ( image == nullptr )
				{
					return {};
				}

				const int                 red   = shiftOf( image->red_mask );
				const int                 green = shiftOf( image->green_mask );
				const int                 blue  = shiftOf( image->blue_mask );
				std::vector<std::uint8_t> rgb( static_cast<std::size_t>( region.width ) * static_cast<std::size_t>( region.height ) * 3 );
				for ( int y = 0; y < region.height; ++y )
				{
					for ( int x = 0; x < region.width; ++x )
					{
						unsigned long pixel = 0;
						if ( image->bits_per_pixel == 32 )
						{
							std::uint32_t raw = 0;
							std::memcpy( &raw, image->data + ( static_cast<std::ptrdiff_t>( y ) * image->bytes_per_line ) + ( static_cast<std::ptrdiff_t>( x ) * 4 ), sizeof( raw ) );
							pixel = raw;
						}
						else
						{
							pixel = XGetPixel( image, x, y );
						}
						const std::size_t at = ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( region.width ) ) + static_cast<std::size_t>( x ) ) * 3;
						rgb[at]              = static_cast<std::uint8_t>( ( pixel & image->red_mask ) >> static_cast<unsigned>( red ) );
						rgb[at + 1]          = static_cast<std::uint8_t>( ( pixel & image->green_mask ) >> static_cast<unsigned>( green ) );
						rgb[at + 2]          = static_cast<std::uint8_t>( ( pixel & image->blue_mask ) >> static_cast<unsigned>( blue ) );
					}
				}
				XDestroyImage( image );
				return rgb;
			}

		private:
			Display* display_;
			Window   root_;
		};

	} // namespace

	Result<std::unique_ptr<ScreenReader>> createX11Screen()
	{
		Display* display = XOpenDisplay( nullptr );
		if ( display == nullptr )
		{
			return fail( "cannot connect to the X server for screen reading" );
		}
		return std::make_unique<X11Screen>( display );
	}

} // namespace lexiglance::platform
