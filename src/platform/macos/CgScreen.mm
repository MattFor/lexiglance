#include "CgScreen.h"

#include <CoreGraphics/CoreGraphics.h>

#include <array>
#include <cmath>
#include <type_traits>

#include <dlfcn.h>

namespace lexiglance::platform
{

	namespace
	{

		struct Release
		{
			void operator()( CFTypeRef object ) const noexcept
			{
				if ( object != nullptr )
				{
					CFRelease( object );
				}
			}
		};

		template <typename T>
		using Cf = std::unique_ptr<std::remove_pointer_t<T>, Release>;

		// ponytail: CGWindowListCreateImage is obsolete from the macOS 15 SDK on, though macOS 15 still has it; looked up at
		// run time so any deployment target builds. ScreenCaptureKit's SCScreenshotManager (macOS 14+, asynchronous) is
		// the replacement once a macOS drops it.
		using CreateImage = CGImageRef ( * )( CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption );

		Rect toRect( const CGRect& box )
		{
			const int left = static_cast<int>( std::floor( CGRectGetMinX( box ) ) );
			const int top  = static_cast<int>( std::floor( CGRectGetMinY( box ) ) );
			return { .x = left, .y = top, .width = static_cast<int>( std::ceil( CGRectGetMaxX( box ) ) ) - left, .height = static_cast<int>( std::ceil( CGRectGetMaxY( box ) ) ) - top };
		}

		CGWindowID pointerWindow()
		{
			const Cf<CFArrayRef> windows( CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenOnly, kCGNullWindowID ) );
			const int            level = CGWindowLevelForKey( kCGCursorWindowLevelKey );
			for ( CFIndex i = 0; windows && i < CFArrayGetCount( windows.get() ); ++i )
			{
				const auto info   = static_cast<CFDictionaryRef>( CFArrayGetValueAtIndex( windows.get(), i ) );
				const auto layer  = static_cast<CFNumberRef>( CFDictionaryGetValue( info, kCGWindowLayer ) );
				const auto number = static_cast<CFNumberRef>( CFDictionaryGetValue( info, kCGWindowNumber ) );
				int        at     = 0;
				CGWindowID id     = kCGNullWindowID;
				if ( layer != nullptr && number != nullptr && CFNumberGetValue( layer, kCFNumberIntType, &at ) && at == level && CFNumberGetValue( number, kCFNumberSInt32Type, &id ) )
				{
					return id;
				}
			}
			return kCGNullWindowID;
		}

		class CgScreen final : public ScreenReader
		{
		public:
			explicit CgScreen( CreateImage create ) :
				create_( create )
			{
			}

			Rect bounds() override
			{
				std::array<CGDirectDisplayID, 32> displays{};
				std::uint32_t                     count = 0;
				CGRect                            all   = CGRectNull;
				if ( CGGetActiveDisplayList( displays.size(), displays.data(), &count ) == kCGErrorSuccess )
				{
					for ( std::uint32_t i = 0; i < count; ++i )
					{
						all = CGRectUnion( all, CGDisplayBounds( displays[i] ) );
					}
				}
				return toRect( CGRectIsNull( all ) ? CGDisplayBounds( CGMainDisplayID() ) : all );
			}

			std::optional<Rect> windowRect( std::uint64_t id ) override
			{
				const Cf<CFArrayRef> windows( CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>( id ) ) );
				if ( !windows || CFArrayGetCount( windows.get() ) == 0 )
				{
					return std::nullopt;
				}
				const auto info  = static_cast<CFDictionaryRef>( CFArrayGetValueAtIndex( windows.get(), 0 ) );
				const auto frame = static_cast<CFDictionaryRef>( CFDictionaryGetValue( info, kCGWindowBounds ) );
				CGRect     rect{};
				if ( frame == nullptr || !CGRectMakeWithDictionaryRepresentation( frame, &rect ) )
				{
					return std::nullopt;
				}
				return toRect( rect );
			}

			std::vector<std::uint8_t> read( const Rect& region ) override
			{
				if ( region.empty() )
				{
					return {};
				}
				// Below the pointer where the window server draws it as a window (larger pointers, virtual machines): read with
				// the rest, it would be part of the character under it.
				const CGWindowID     pointer = pointerWindow();
				const Cf<CGImageRef> image( create_( CGRectMake( region.x, region.y, region.width, region.height ), pointer != kCGNullWindowID ? kCGWindowListOptionOnScreenBelowWindow : kCGWindowListOptionOnScreenOnly, pointer, kCGWindowImageNominalResolution ) );
				if ( !image )
				{
					return {};
				}
				// Drawn into RGBX the size of the region in points: Retina pixels are scaled down, any pixel format converted.
				const auto                width  = static_cast<std::size_t>( region.width );
				const auto                height = static_cast<std::size_t>( region.height );
				std::vector<std::uint8_t> rgbx( width * height * 4 );
				CGColorSpaceRef           space = CGImageGetColorSpace( image.get() );
				const Cf<CGColorSpaceRef> device( CGColorSpaceCreateDeviceRGB() );
				if ( space == nullptr || CGColorSpaceGetModel( space ) != kCGColorSpaceModelRGB )
				{
					space = device.get();
				}
				const Cf<CGContextRef> context( CGBitmapContextCreate( rgbx.data(), width, height, 8, width * 4, space, kCGImageAlphaNoneSkipLast ) );
				if ( !context )
				{
					return {};
				}
				CGContextSetInterpolationQuality( context.get(), kCGInterpolationHigh );
				CGContextDrawImage( context.get(), CGRectMake( 0, 0, static_cast<CGFloat>( width ), static_cast<CGFloat>( height ) ), image.get() );
				std::vector<std::uint8_t> rgb( width * height * 3 );
				for ( std::size_t i = 0; i < width * height; ++i )
				{
					rgb[i * 3]         = rgbx[i * 4];
					rgb[( i * 3 ) + 1] = rgbx[( i * 4 ) + 1];
					rgb[( i * 3 ) + 2] = rgbx[( i * 4 ) + 2];
				}
				return rgb;
			}

		private:
			CreateImage create_;
		};

	} // namespace

	Result<std::unique_ptr<ScreenReader>> createCgScreen()
	{
		static const auto create = reinterpret_cast<CreateImage>( dlsym( RTLD_DEFAULT, "CGWindowListCreateImage" ) );
		if ( create == nullptr )
		{
			return fail( "this macOS no longer offers the screen reading Lexiglance uses (CGWindowListCreateImage)" );
		}
		// Without it macOS shows only the desktop picture and Lexiglance's own windows, which OCR would read as nothing.
		if ( !CGPreflightScreenCaptureAccess() )
		{
			// macOS asks only the first time; afterwards this just answers no.
			( void )CGRequestScreenCaptureAccess();
			return fail(
					"macOS has not given Lexiglance {}, so OCR cannot read other applications' windows. Turn Lexiglance on in System Settings, "
					"Privacy & Security, Screen & System Audio Recording, then restart Lexiglance.",
					screen_permission_absent
			);
		}
		return std::make_unique<CgScreen>( create );
	}

} // namespace lexiglance::platform
