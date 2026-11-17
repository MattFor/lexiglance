#include "MacBackend.h"

#include "../Capture.h"
#include "../ocr/OcrCapture.h"
#include "AxCapture.h"
#include "CgScreen.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Thread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cairo.h>
#include <unistd.h>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <QuartzCore/QuartzCore.h>
#include <IOKit/hidsystem/IOLLEvent.h>

// Never key, never main: clicks reach it without the application below losing the keyboard.
@interface LGPanel : NSPanel
@end

@implementation LGPanel
- ( BOOL )canBecomeKeyWindow
{
	return NO;
}

- ( BOOL )canBecomeMainWindow
{
	return NO;
}
@end

// Shows a canvas through its layer and passes the mouse on; y runs down, like the popup's pixels.
@interface LGView : NSView
@property( nonatomic, copy ) void ( ^onEvent )( NSEvent* );
@end

@implementation LGView
- ( BOOL )isFlipped
{
	return YES;
}

- ( BOOL )acceptsFirstMouse:( NSEvent* )event
{
	return YES;
}

- ( void )forward:( NSEvent* )event
{
	if ( self.onEvent != nil )
	{
		self.onEvent( event );
	}
}

- ( void )mouseDown:( NSEvent* )event
{
	[self forward:event];
}

- ( void )rightMouseDown:( NSEvent* )event
{
	[self forward:event];
}

- ( void )otherMouseDown:( NSEvent* )event
{
	[self forward:event];
}

- ( void )rightMouseDragged:( NSEvent* )event
{
	[self forward:event];
}

- ( void )otherMouseDragged:( NSEvent* )event
{
	[self forward:event];
}

- ( void )rightMouseUp:( NSEvent* )event
{
	[self forward:event];
}

- ( void )otherMouseUp:( NSEvent* )event
{
	[self forward:event];
}

- ( void )scrollWheel:( NSEvent* )event
{
	[self forward:event];
}
@end

// Asked to quit (logging out, Activity Monitor) and told when the appearance changes.
@interface LGAppDelegate : NSObject <NSApplicationDelegate>
@property( nonatomic, copy ) NSApplicationTerminateReply ( ^onTerminate )( void );
@property( nonatomic, copy ) void ( ^onThemeChange )( void );
@end

@implementation LGAppDelegate
- ( NSApplicationTerminateReply )applicationShouldTerminate:( NSApplication* )sender
{
	return self.onTerminate != nil ? self.onTerminate() : NSTerminateNow;
}

- ( void )themeChanged:( NSNotification* )notification
{
	if ( self.onThemeChange != nil )
	{
		self.onThemeChange();
	}
}
@end

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// As on Windows: pointer speed (points per second) above which nothing is looked up, and the pause after it.
		constexpr double fast_pointer    = 1500.0;
		constexpr auto   settle_time     = std::chrono::milliseconds( 40 );
		constexpr auto   source_interval = std::chrono::milliseconds( 250 );
		constexpr auto   input_interval  = std::chrono::seconds( 1 );
		// macOS tells nobody when the pasteboard changes: its change count is looked at this often.
		constexpr auto pasteboard_interval = std::chrono::milliseconds( 200 );
		// ponytail: trackpad and Magic Mouse scrolling counts a notch every this many points; tune on real hardware.
		constexpr double points_per_notch = 40.0;
		constexpr short  wake_subtype     = 0x4C47;

		// Virtual key codes (Carbon's kVK_ names), which the event tap reports.
		struct KeyCode
		{
			CGKeyCode   code;
			config::Key key;
			// The modifier's own bit in the event flags (NX_DEVICE*), 0 for other keys.
			CGEventFlags device;
			CGEventFlags either;
		};

		// ponytail: Caps Lock is left out: macOS reports its lock state, never its release.
		constexpr std::array<KeyCode, 24> key_codes{ {
				{ .code = 0x38, .key = config::Key::ShiftL, .device = NX_DEVICELSHIFTKEYMASK, .either = kCGEventFlagMaskShift },
				{ .code = 0x3C, .key = config::Key::ShiftR, .device = NX_DEVICERSHIFTKEYMASK, .either = kCGEventFlagMaskShift },
				{ .code = 0x3B, .key = config::Key::ControlL, .device = NX_DEVICELCTLKEYMASK, .either = kCGEventFlagMaskControl },
				{ .code = 0x3E, .key = config::Key::ControlR, .device = NX_DEVICERCTLKEYMASK, .either = kCGEventFlagMaskControl },
				{ .code = 0x3A, .key = config::Key::AltL, .device = NX_DEVICELALTKEYMASK, .either = kCGEventFlagMaskAlternate },
				{ .code = 0x3D, .key = config::Key::AltR, .device = NX_DEVICERALTKEYMASK, .either = kCGEventFlagMaskAlternate },
				{ .code = 0x37, .key = config::Key::SuperL, .device = NX_DEVICELCMDKEYMASK, .either = kCGEventFlagMaskCommand },
				{ .code = 0x36, .key = config::Key::SuperR, .device = NX_DEVICERCMDKEYMASK, .either = kCGEventFlagMaskCommand },
				{ .code = 0x6E, .key = config::Key::Menu, .device = 0, .either = 0 },
				{ .code = 0x31, .key = config::Key::Space, .device = 0, .either = 0 },
				{ .code = 0x30, .key = config::Key::Tab, .device = 0, .either = 0 },
				{ .code = 0x35, .key = config::Key::Escape, .device = 0, .either = 0 },
				{ .code = 0x7A, .key = config::Key::F1, .device = 0, .either = 0 },
				{ .code = 0x78, .key = config::Key::F2, .device = 0, .either = 0 },
				{ .code = 0x63, .key = config::Key::F3, .device = 0, .either = 0 },
				{ .code = 0x76, .key = config::Key::F4, .device = 0, .either = 0 },
				{ .code = 0x60, .key = config::Key::F5, .device = 0, .either = 0 },
				{ .code = 0x61, .key = config::Key::F6, .device = 0, .either = 0 },
				{ .code = 0x62, .key = config::Key::F7, .device = 0, .either = 0 },
				{ .code = 0x64, .key = config::Key::F8, .device = 0, .either = 0 },
				{ .code = 0x65, .key = config::Key::F9, .device = 0, .either = 0 },
				{ .code = 0x6D, .key = config::Key::F10, .device = 0, .either = 0 },
				{ .code = 0x67, .key = config::Key::F11, .device = 0, .either = 0 },
				{ .code = 0x6F, .key = config::Key::F12, .device = 0, .either = 0 },
		} };

		const KeyCode* keyCodeOf( CGKeyCode code )
		{
			const auto* it = std::ranges::find( key_codes, code, &KeyCode::code );
			return it != key_codes.end() ? &*it : nullptr;
		}

		// Whether a modifier is down by the flags of an event or of the keyboard. Left and right have a bit each; a keyboard
		// that sets neither has only the shared one.
		bool modifierHeld( const KeyCode& code, CGEventFlags flags )
		{
			const CGEventFlags sides = code.either == kCGEventFlagMaskShift     ? ( NX_DEVICELSHIFTKEYMASK | NX_DEVICERSHIFTKEYMASK )
			                         : code.either == kCGEventFlagMaskControl   ? ( NX_DEVICELCTLKEYMASK | NX_DEVICERCTLKEYMASK )
			                         : code.either == kCGEventFlagMaskAlternate ? ( NX_DEVICELALTKEYMASK | NX_DEVICERALTKEYMASK )
			                                                                    : ( NX_DEVICELCMDKEYMASK | NX_DEVICERCMDKEYMASK );
			return ( flags & sides ) != 0 ? ( flags & code.device ) != 0 : ( flags & code.either ) != 0;
		}

		struct MouseButton
		{
			std::int64_t number;
			config::Key  key;
		};

		constexpr std::array<MouseButton, 5> mouse_buttons{ {
				{ .number = 0, .key = config::Key::MouseLeft },
				{ .number = 1, .key = config::Key::MouseRight },
				{ .number = 2, .key = config::Key::MouseMiddle },
				{ .number = 3, .key = config::Key::MouseBack },
				{ .number = 4, .key = config::Key::MouseForward },
		} };

		constexpr int key_count = static_cast<int>( config::Key::MouseForward ) + 1;

		// Quartz coordinates (y down from the top of the main display) of an AppKit rectangle and back.
		CGFloat mainHeight()
		{
			NSArray<NSScreen*>* screens = NSScreen.screens;
			return screens.count > 0 ? screens[0].frame.size.height : 0.0;
		}

		Rect quartzRect( NSRect r )
		{
			return { .x = static_cast<int>( std::lround( r.origin.x ) ), .y = static_cast<int>( std::lround( mainHeight() - r.origin.y - r.size.height ) ), .width = static_cast<int>( std::lround( r.size.width ) ), .height = static_cast<int>( std::lround( r.size.height ) ) };
		}

		NSRect cocoaRect( double x, double y, double width, double height )
		{
			return NSMakeRect( x, mainHeight() - y - height, width, height );
		}

		NSScreen* screenAt( Point point )
		{
			for ( NSScreen* screen in NSScreen.screens )
			{
				if ( quartzRect( screen.frame ).contains( point ) )
				{
					return screen;
				}
			}
			return NSScreen.mainScreen;
		}

		bool darkAppearance()
		{
			NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
			return style != nil && [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame;
		}

		std::uint32_t premultiplied( const render::Color& color, double alpha ) noexcept
		{
			const auto channel = [&]( double value ) { return static_cast<std::uint32_t>( std::lround( std::clamp( value * alpha, 0.0, 1.0 ) * 255.0 ) ); };
			return ( channel( 1.0 ) << 24U ) | ( channel( color.r ) << 16U ) | ( channel( color.g ) << 8U ) | channel( color.b );
		}

		// A Cairo image (premultiplied ARGB, in pixels) shown as the contents of a view's layer.
		class Canvas
		{
		public:
			Canvas() = default;

			~Canvas()
			{
				if ( surface_ != nullptr )
				{
					cairo_surface_destroy( surface_ );
				}
			}

			Canvas( const Canvas& )            = delete;
			Canvas& operator=( const Canvas& ) = delete;
			Canvas( Canvas&& )                 = delete;
			Canvas& operator=( Canvas&& )      = delete;

			cairo_surface_t* resize( int width, int height )
			{
				width  = std::max( 1, width );
				height = std::max( 1, height );
				if ( surface_ != nullptr && width == width_ && height == height_ )
				{
					return surface_;
				}
				if ( surface_ != nullptr )
				{
					cairo_surface_destroy( surface_ );
				}
				surface_ = cairo_image_surface_create( CAIRO_FORMAT_ARGB32, width, height );
				if ( cairo_surface_status( surface_ ) != CAIRO_STATUS_SUCCESS )
				{
					cairo_surface_destroy( surface_ );
					surface_ = nullptr;
					return nullptr;
				}
				width_  = width;
				height_ = height;
				return surface_;
			}

			[[nodiscard]] cairo_surface_t* surface() const noexcept
			{
				return surface_;
			}

			// Premultiplied 0xAARRGGBB pixels, row by row (stride() of them a row).
			[[nodiscard]] std::uint32_t* pixels() const noexcept
			{
				return surface_ != nullptr ? reinterpret_cast<std::uint32_t*>( cairo_image_surface_get_data( surface_ ) ) : nullptr;
			}

			[[nodiscard]] int stride() const noexcept
			{
				return surface_ != nullptr ? cairo_image_surface_get_stride( surface_ ) / 4 : 0;
			}

			void present( NSView* view ) const
			{
				if ( surface_ == nullptr )
				{
					return;
				}
				cairo_surface_flush( surface_ );
				const int  stride = cairo_image_surface_get_stride( surface_ );
				CFDataRef  bytes  = CFDataCreate( nullptr, cairo_image_surface_get_data( surface_ ), static_cast<CFIndex>( stride ) * height_ );
				auto*      source = CGDataProviderCreateWithCFData( bytes );
				auto*      space  = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
				CGImageRef image  = CGImageCreate( static_cast<std::size_t>( width_ ), static_cast<std::size_t>( height_ ), 8, 32, static_cast<std::size_t>( stride ), space, static_cast<CGBitmapInfo>( kCGImageAlphaPremultipliedFirst ) | kCGBitmapByteOrder32Little, source, nullptr, false, kCGRenderingIntentDefault );
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				view.layer.contents = ( __bridge id )image;
				[CATransaction commit];
				CGImageRelease( image );
				CGColorSpaceRelease( space );
				CGDataProviderRelease( source );
				CFRelease( bytes );
			}

		private:
			cairo_surface_t* surface_ = nullptr;
			int              width_   = 0;
			int              height_  = 0;
		};

		LGPanel* makePanel( LGView* view, bool clickable )
		{
			LGPanel* panel           = [[LGPanel alloc] initWithContentRect:NSMakeRect( 0, 0, 1, 1 ) styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
			panel.opaque             = NO;
			panel.backgroundColor    = NSColor.clearColor;
			panel.hasShadow          = NO;
			panel.hidesOnDeactivate  = NO;
			panel.releasedWhenClosed = NO;
			panel.ignoresMouseEvents = !clickable;
			panel.animationBehavior  = NSWindowAnimationBehaviorNone;
			panel.level              = NSPopUpMenuWindowLevel + ( clickable ? 1 : 0 );
			// On every Space and over full-screen applications, never in Mission Control or the window cycle.
			panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
			// Layer-hosting: AppKit leaves its contents alone.
			view.layer                 = [CALayer layer];
			view.wantsLayer            = YES;
			view.layer.contentsGravity = kCAGravityResize;
			panel.contentView          = view;
			return panel;
		}

		class MacBackend final : public Backend
		{
		public:
			MacBackend() = default;

			~MacBackend() override
			{
				// The wheel's thread lets go of its tap first.
				wheel_thread_ = {};
				for ( CFMachPortRef* tap : { &tap_, &wheel_tap_ } )
				{
					if ( *tap != nullptr )
					{
						CGEventTapEnable( *tap, false );
						CFMachPortInvalidate( *tap );
						CFRelease( *tap );
						*tap = nullptr;
					}
				}
				if ( tap_source_ != nullptr )
				{
					CFRunLoopRemoveSource( CFRunLoopGetMain(), tap_source_, kCFRunLoopCommonModes );
					CFRelease( tap_source_ );
				}
				for ( id observer : workspace_observers_ )
				{
					[NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer];
				}
				for ( id observer : observers_ )
				{
					[NSNotificationCenter.defaultCenter removeObserver:observer];
				}
				[NSDistributedNotificationCenter.defaultCenter removeObserver:delegate_];
				[popup_window_ orderOut:nil];
				[highlight_window_ orderOut:nil];
			}

			MacBackend( const MacBackend& )            = delete;
			MacBackend& operator=( const MacBackend& ) = delete;
			MacBackend( MacBackend&& )                 = delete;
			MacBackend& operator=( MacBackend&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "macos";
			}

			Result<> open()
			{
				if ( ![NSThread isMainThread] )
				{
					return fail( "the macOS backend must be created on the main thread" );
				}
				[NSApplication sharedApplication];
				// No Dock icon and no menu bar; never becomes the active application.
				[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
				delegate_             = [LGAppDelegate new];
				delegate_.onTerminate = ^NSApplicationTerminateReply {
				  // Logging out, restarting or shutting down gives a reason: as on Windows, the process ends once the daemon was
				  // told. Any other quit ends the daemon as SIGTERM does.
				  if ( [NSAppleEventManager.sharedAppleEventManager.currentAppleEvent paramDescriptorForKeyword:kAEQuitReason] != nil )
				  {
					  sessionEnding();
					  std::_Exit( 0 );
				  }
				  quit();
				  return NSTerminateCancel;
				};
				delegate_.onThemeChange = ^{
				  loadDesktopSettings();
				};
				NSApp.delegate = delegate_;
				[NSApp finishLaunching];
				// ponytail: unproven in the VM (processes started over SSH are not napped); App Nap could hold the scan's timer back by
				// seconds while no window of ours is on screen.
				activity_ = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"Looks up words under the pointer"];

				loadDesktopSettings();
				own_pasteboard_  = NSPasteboard.generalPasteboard.changeCount;
				pasteboard_seen_ = own_pasteboard_;

				NSNotificationCenter* workspace = NSWorkspace.sharedWorkspace.notificationCenter;
				workspace_observers_.push_back( [workspace addObserverForName:NSWorkspaceWillPowerOffNotification
																	   object:nil
																		queue:nil
																   usingBlock:^( NSNotification* ) {
																	 sessionEnding();
																   }] );
				observers_.push_back( [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification
																					  object:nil
																					   queue:nil
																				  usingBlock:^( NSNotification* ) {
																					loadDesktopSettings();
																				  }] );
				// Distributed notifications wait while an application is inactive, and this one never becomes active.
				[NSDistributedNotificationCenter.defaultCenter addObserver:delegate_
																  selector:@selector( themeChanged: )
																	  name:@"AppleInterfaceThemeChangedNotification"
																	object:nil
														suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
				return {};
			}

			Result<> start( Events events ) override
			{
				events_ = std::move( events );
				// Asked once a start, never again while running: macOS shows its prompt only the first time anyway. Accessibility and
				// Screen Recording are asked for by the captures that need them.
				if ( !createTap() )
				{
					log::warn( "input: Input Monitoring is not allowed, so the trigger cannot be seen; asking for it, and listening once it is allowed" );
					( void )CGRequestListenEventAccess();
				}
				syncKeyboard();
				return {};
			}

			void run() override
			{
				while ( !quit_.load( std::memory_order_acquire ) )
				{
					// What AppKit autoreleases while input is handled goes with each turn, not at the end of the run.
					@autoreleasepool
					{
						turn();
					}
				}
				// Scrolling must never wait for a loop that no longer runs.
				lockWheel( false );
			}

			void quit() override
			{
				quit_.store( true, std::memory_order_release );
				wake();
			}

			void post( Task task ) override
			{
				{
					const std::scoped_lock lock( tasks_mutex_ );
					tasks_.push_back( std::move( task ) );
				}
				wake();
			}

			void configure( const config::Config& config ) override
			{
				if ( auto chord = config::parseChord( config.scan.trigger ) )
				{
					chord_ = std::move( *chord );
				}
				sentence_key_   = config.translation.enabled ? config::extraKey( config.translation.sentence_key, chord_ ) : config::KeyGroup{};
				delay_          = std::chrono::milliseconds( config.scan.delay_ms );
				move_threshold_ = config.scan.move_threshold;
				popup_offset_x_ = config.popup.offset_x;
				popup_offset_y_ = config.popup.offset_y;
				select_middle_  = config.popup.select_button == config::MouseButton::Middle;
				highlight_auto_ = config.popup.highlight_auto;
				wheel_length_   = config.scan.wheel_length;
				wheel_lock_     = config.scan.wheel_lock;
				ocr_            = config.scan.ocr != config::OcrMode::Off;
				if ( compositor_mode_ != config.popup.compositor )
				{
					compositor_mode_ = config.popup.compositor;
					hidePopup();
					hideHighlight();
					// macOS always composites; only "off" asks for the plain look.
					transparent_.store( compositor_mode_ != config::Compositor::Off, std::memory_order_relaxed );
					publishOverlays();
				}
				if ( const auto look = lookFor( config.popup ); look != look_ )
				{
					look_ = look;
					hideHighlight();
				}
				placement_ = config.popup.placement;
				// Copied text stands in for X11's selection, as on Windows.
				if ( selection_mode_ == config::SelectionMode::Off && config.scan.selection != config::SelectionMode::Off )
				{
					pasteboard_seen_ = NSPasteboard.generalPasteboard.changeCount;
				}
				selection_mode_ = config.scan.selection;
				syncKeyboard();
				if ( !recording_ )
				{
					updateTrigger();
				}
				updateWheelLock();
			}

			void showPopup( PopupContent content ) override
			{
				if ( !content.image )
				{
					return;
				}
				const Rect before = popup_shown_ ? popup_rect_ : Rect{};
				watchSource( content.source, true );
				popup_     = std::move( content );
				scroll_    = 0;
				selecting_ = false;
				select_anchor_.reset();
				select_focus_.reset();
				popup_size_       = render::popupSize( *popup_.image, popup_.style );
				const Rect  a     = popup_.anchor;
				const Point where = { .x = a.x + ( a.width / 2 ), .y = a.y + ( a.height / 2 ) };
				// A pixel of the image is a pixel of the display, sharp whatever the scale it was drawn at (the desktop's, or the
				// popup scale setting, which on every platform counts device pixels); the window is in points.
				popup_backing_ = backingAt( where );
				const double w = popup_size_.width / popup_backing_;
				const double h = popup_size_.height / popup_backing_;

				const Rect   monitor    = monitorAt( where );
				const double gap        = popup_.style.px( popup_offset_y_ ) / popup_backing_;
				double       x          = a.x + ( popup_.style.px( popup_offset_x_ ) / popup_backing_ );
				const double below      = a.y + a.height + gap;
				const double above      = a.y - gap - h;
				const bool   fits_below = below + h <= monitor.y + monitor.height;
				const bool   fits_above = above >= monitor.y;
				const double last       = std::max<double>( monitor.y, monitor.y + monitor.height - h );
				double       y          = 0;
				if ( placement_ == config::PopupPlacement::AboveText )
				{
					y = fits_above ? above : fits_below ? below
					                                    : last;
				}
				else
				{
					y = fits_below ? below : fits_above ? above
					                                    : last;
				}
				x = std::clamp<double>( x, monitor.x, std::max<double>( monitor.x, monitor.x + monitor.width - w ) );

				ensurePopupWindow();
				popup_rect_ = { .x = static_cast<int>( std::floor( x ) ), .y = static_cast<int>( std::floor( y ) ), .width = static_cast<int>( std::ceil( w ) ), .height = static_cast<int>( std::ceil( h ) ) };
				[popup_window_ setFrame:cocoaRect( x, y, w, h ) display:NO];
				popup_canvas_.resize( popup_size_.width, popup_size_.height );
				paintPopup( true );
				[popup_window_ orderFrontRegardless];
				popup_shown_ = true;
				updateWheelLock();
				vacate( before );
				publishOverlays();
			}

			void hidePopup() override
			{
				watchSource( 0 );
				if ( popup_shown_ )
				{
					[popup_window_ orderOut:nil];
					popup_shown_ = false;
					updateWheelLock();
					selecting_ = false;
					popup_.image.reset();
					vacate( popup_rect_ );
					publishOverlays();
				}
			}

			[[nodiscard]] bool popupVisible() const override
			{
				return popup_shown_;
			}

			[[nodiscard]] bool pointerOnPopup() override
			{
				return popup_shown_ && popup_rect_.contains( pointer() );
			}

			void showHighlight( std::span<const Rect> rects, const render::Color& color ) override
			{
				if ( rects.empty() || std::ranges::all_of( rects, &Rect::empty ) )
				{
					return;
				}
				const Rect before = highlight_shown_ ? highlight_rect_ : Rect{};
				ensureHighlightWindow();
				highlight_color_ = color;
				setHighlightPieces( rects );
				highlight_lines_ = render::highlightDepth( look_ );
				const bool fill  = look_.shape == render::HighlightShape::Fill;
				if ( highlight_auto_ || ( fill && !transparent_.load( std::memory_order_relaxed ) ) )
				{
					captureUnderlay( highlight_rect_ );
				}
				if ( highlight_auto_ )
				{
					highlight_color_ = render::autoHighlight( underlay_, color.a, fill );
				}
				const Rect& r    = highlight_rect_;
				highlight_scale_ = backingAt( { .x = r.x + ( r.width / 2 ), .y = r.y + ( r.height / 2 ) } );
				[highlight_window_ setFrame:cocoaRect( r.x, r.y, r.width, r.height ) display:NO];
				highlight_canvas_.resize( static_cast<int>( std::ceil( r.width * highlight_scale_ ) ), static_cast<int>( std::ceil( r.height * highlight_scale_ ) ) );
				paintHighlight();
				[highlight_window_ orderFrontRegardless];
				highlight_shown_ = true;
				vacate( before );
				publishOverlays();
			}

			void hideHighlight() override
			{
				if ( highlight_shown_ )
				{
					[highlight_window_ orderOut:nil];
					highlight_shown_ = false;
					vacate( highlight_rect_ );
					underlay_.clear();
					underlay_gray_.reset();
					marker_ = false;
					publishOverlays();
				}
			}

			void showBadge( std::string text, std::chrono::milliseconds duration ) override
			{
				badge_       = std::move( text );
				badge_until_ = Clock::now() + duration;
				paintPopup();
			}

			Point pointer() override
			{
				CGEventRef event = CGEventCreate( nullptr );
				if ( event == nullptr )
				{
					return {};
				}
				const CGPoint at = CGEventGetLocation( event );
				CFRelease( event );
				return { .x = static_cast<int>( std::floor( at.x ) ), .y = static_cast<int>( std::floor( at.y ) ) };
			}

			WindowInfo windowAt( Point point ) override
			{
				WindowInfo info;
				@autoreleasepool
				{
					NSArray*   windows = CFBridgingRelease( CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID ) );
					const auto ours    = static_cast<std::uint32_t>( getpid() );
					// Front to back: the first window of a normal application under the point.
					for ( NSDictionary* window in windows )
					{
						NSDictionary* frame  = window[( __bridge NSString* )kCGWindowBounds];
						CGRect        bounds = CGRectNull;
						if ( frame == nil || !CGRectMakeWithDictionaryRepresentation( ( __bridge CFDictionaryRef )frame, &bounds ) || !CGRectContainsPoint( bounds, CGPointMake( point.x + 0.5, point.y + 0.5 ) ) )
						{
							continue;
						}
						const auto number = [window[( __bridge NSString* )kCGWindowNumber] unsignedLongLongValue];
						const auto pid    = [window[( __bridge NSString* )kCGWindowOwnerPID] unsignedIntValue];
						if ( pid == ours )
						{
							// The highlight lets the pointer through to the text below; the popup does not.
							if ( popup_shown_ && number == static_cast<std::uint64_t>( popup_window_.windowNumber ) )
							{
								info.id  = number;
								info.pid = pid;
								info.own = true;
								return info;
							}
							continue;
						}
						if ( [window[( __bridge NSString* )kCGWindowLayer] intValue] != 0 || [window[( __bridge NSString* )kCGWindowAlpha] doubleValue] <= 0.0 )
						{
							continue;
						}
						info.id                   = number;
						info.pid                  = pid;
						info.wm_class             = programOf( pid, window[( __bridge NSString* )kCGWindowOwnerName] );
						info.active               = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == static_cast<pid_t>( pid );
						CGDirectDisplayID display = 0;
						std::uint32_t     count   = 0;
						if ( CGGetDisplaysWithPoint( CGPointMake( point.x, point.y ), 1, &display, &count ) == kCGErrorSuccess && count > 0 )
						{
							info.fullscreen = CGRectContainsRect( bounds, CGDisplayBounds( display ) );
						}
						return info;
					}
				}
				return info;
			}

			void recordChord( std::function<void( std::vector<std::string> )> done ) override
			{
				recording_ = std::move( done );
				recorded_.clear();
				syncKeyboard();
			}

			void cancelRecording() override
			{
				recording_ = nullptr;
				recorded_.clear();
			}

			[[nodiscard]] double scaleFactor() const override
			{
				return scale_.load( std::memory_order_relaxed );
			}

			[[nodiscard]] bool prefersDarkTheme() const override
			{
				return dark_.load( std::memory_order_relaxed );
			}

			[[nodiscard]] bool supportsTransparency() const override
			{
				return transparent_.load( std::memory_order_relaxed );
			}

			[[nodiscard]] Clock::time_point lastInput() const override
			{
				return Clock::time_point( Clock::duration( last_input_.load( std::memory_order_relaxed ) ) );
			}

			void diagnose( std::vector<health::Check>& out, bool interactive ) override
			{
				std::string chord;
				for ( const config::KeyGroup& group : chord_ )
				{
					std::string names;
					for ( const config::Key key : group )
					{
						names.append( names.empty() ? "" : " or " ).append( config::displayKeyName( key ) );
					}
					chord.append( chord.empty() ? "" : " + " ).append( names );
				}
				health::Check trigger{ .id = "trigger", .title = "Trigger" };
				if ( chord_.empty() )
				{
					trigger.status = health::Severity::Error;
					trigger.detail = "No trigger is set, so nothing can be looked up. Choose one on the Scanning page.";
					trigger.fix    = "open-scanning";
				}
				else
				{
					trigger.detail = std::format( "Hold {} and point at a word{}.", chord, chordHeld() ? " (held right now)" : "" );
				}
				out.push_back( std::move( trigger ) );

				const auto recorded = down_;
				syncKeyboard();
				if ( recorded != down_ )
				{
					updateTrigger();
					out.push_back(
							{ .id     = "keyboard",
							  .title  = "Keyboard state",
							  .status = health::Severity::Info,
							  .detail = "Lexiglance thought a key was held when it wasn't (or the other way round). That's fixed now." }
					);
				}

				const bool listen = CGPreflightListenEventAccess();
				if ( !listen )
				{
					out.push_back(
							{ .id     = "input-monitoring",
							  .title  = "Input Monitoring",
							  .status = health::Severity::Error,
							  .detail = "macOS isn't letting Lexiglance see the keyboard, so it never sees the trigger and nothing is looked up. Turn "
							            "Lexiglance on in System Settings -> Privacy & Security -> Input Monitoring, then restart Lexiglance (macOS "
							            "only applies it to programs started afterwards).",
							  .fix    = "open-privacy-input-monitoring" }
					);
				}
				if ( tap_ == nullptr )
				{
					( void )createTap();
				}

				health::Check input{ .id = "input", .title = "Keyboard and mouse events" };
				if ( tap_ == nullptr )
				{
					input.status = health::Severity::Error;
					input.detail = listen ? "Input Monitoring is allowed, but macOS isn't passing keys and clicks to Lexiglance yet. Restart Lexiglance."
					                      : "None arrive until Input Monitoring is allowed (see above).";
					input.fix    = listen ? "restart" : "";
				}
				else if ( !CGEventTapIsEnabled( tap_ ) )
				{
					CGEventTapEnable( tap_, true );
					input.status = health::Severity::Warning;
					input.detail = "macOS had stopped passing keyboard and mouse events to Lexiglance, so it couldn't see the trigger. They're coming through again.";
				}
				else
				{
					const auto last = lastInput();
					const auto age  = last == Clock::time_point{} ? -1 : std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - last ).count();
					if ( age < 0 )
					{
						input.status = interactive ? health::Severity::Error : health::Severity::Info;
						input.detail = interactive ? "No keyboard or mouse events reached Lexiglance, not even the click that started this check. Restart Lexiglance."
						                           : "None have arrived since Lexiglance started. Move the mouse or press a key, then check again.";
						input.fix    = interactive ? "restart" : "";
					}
					else if ( interactive && age > 10 )
					{
						input.status = health::Severity::Error;
						input.detail = std::format( "The click that started this check didn't reach Lexiglance (the last event arrived {} s ago). Restart Lexiglance.", age );
						input.fix    = "restart";
					}
					else
					{
						input.detail = age < 2 ? std::string( "Arriving through an event tap." ) : std::format( "Arriving through an event tap (the last one {} s ago).", age );
					}
				}
				out.push_back( std::move( input ) );

				// Accessibility and Screen Recording are the captures' checks.
				const NSRect                   frame   = NSScreen.mainScreen.frame;
				const NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
				out.push_back(
						{ .id     = "desktop",
						  .title  = "Desktop",
						  .detail = std::format(
								  "macOS {}.{}.{}, {}x{} points, scale {}x, {}.",
								  version.majorVersion,
								  version.minorVersion,
								  version.patchVersion,
								  frame.size.width,
								  frame.size.height,
								  scaleFactor(),
								  supportsTransparency() ? "translucent overlays" : "plain overlays (compositor setting off)"
						  ) }
				);
			}

			std::unique_ptr<TextCapture> createTextCapture( const config::Config& config ) override
			{
				std::unique_ptr<TextCapture> accessibility = createAxCapture();
				std::unique_ptr<TextCapture> ocr;
				std::string                  ocr_state = "OCR off";
				if ( config.scan.ocr != config::OcrMode::Off )
				{
					auto created = createOcrCapture(
							{ .engine   = config.scan.ocr_engine,
							  .vertical = config.scan.ocr_vertical,
							  .model    = config.scan.ocr_model,
							  // CgScreen reads a sample a point, whatever the display's scale.
							  .scale     = 1.0,
							  .languages = lang::enabledLanguages( config.disabled_languages ),
							  .overlays  = [this] {
								  const std::scoped_lock lock( overlays_mutex_ );
								  return overlays_; },
							  .screen    = [] { return createCgScreen(); } }
					);
					if ( created )
					{
						ocr = std::move( *created );
					}
					else
					{
						ocr_state = "OCR unavailable: " + created.error().message;
					}
				}
				return std::make_unique<ChainCapture>( std::move( accessibility ), std::move( ocr ), config.scan.ocr, config.scan.ocr_windows, std::move( ocr_state ) );
			}

		private:
			// One turn of the loop: the tasks posted, a wait for an event or the next deadline, and what is due.
			void turn()
			{
				wake_pending_.store( false, std::memory_order_release );
				runTasks();
				if ( quit_.load( std::memory_order_acquire ) )
				{
					return;
				}

				const auto now   = Clock::now();
				auto       until = now + input_interval;
				const auto by    = [&]( Clock::time_point when ) { until = std::min( until, when ); };
				if ( scan_pending_ )
				{
					by( scan_deadline_ );
				}
				if ( badge_until_ != Clock::time_point{} )
				{
					by( badge_until_ );
				}
				if ( watching() )
				{
					by( source_checked_ + source_interval );
				}
				if ( selection_mode_ != config::SelectionMode::Off )
				{
					by( pasteboard_checked_ + pasteboard_interval );
				}
				by( input_checked_ + input_interval );
				waiting_until_ = until;
				NSDate*  date  = [NSDate dateWithTimeIntervalSinceNow:std::max( 0.0, std::chrono::duration<double>( until - now ).count() )];
				NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:date inMode:NSDefaultRunLoopMode dequeue:YES];
				if ( event != nil && !( event.type == NSEventTypeApplicationDefined && event.subtype == wake_subtype ) )
				{
					[NSApp sendEvent:event];
				}
				waiting_until_ = Clock::time_point::max();

				if ( scan_pending_ && Clock::now() >= scan_deadline_ )
				{
					scan( false );
				}
				if ( badge_until_ != Clock::time_point{} && Clock::now() >= badge_until_ )
				{
					badge_until_ = {};
					paintPopup();
				}
				if ( watching() && Clock::now() >= source_checked_ + source_interval )
				{
					checkSource();
				}
				if ( selection_mode_ != config::SelectionMode::Off && Clock::now() >= pasteboard_checked_ + pasteboard_interval )
				{
					pasteboardChanged();
				}
				if ( Clock::now() >= input_checked_ + input_interval )
				{
					checkInput();
				}
			}

			// A listen-only tap: macOS hands Lexiglance a copy of every key, click and turn of the wheel, and nothing is
			// held up, changed or taken away (docs/anticheat.md). Needs Input Monitoring: without it the tap is created
			// but kept switched off.
			bool createTap()
			{
				if ( !CGPreflightListenEventAccess() )
				{
					return false;
				}
				const CGEventMask mask = CGEventMaskBit( kCGEventKeyDown ) | CGEventMaskBit( kCGEventKeyUp ) | CGEventMaskBit( kCGEventFlagsChanged ) | CGEventMaskBit( kCGEventMouseMoved ) |
				                         CGEventMaskBit( kCGEventLeftMouseDown ) | CGEventMaskBit( kCGEventLeftMouseUp ) | CGEventMaskBit( kCGEventRightMouseDown ) | CGEventMaskBit( kCGEventRightMouseUp ) |
				                         CGEventMaskBit( kCGEventOtherMouseDown ) | CGEventMaskBit( kCGEventOtherMouseUp ) | CGEventMaskBit( kCGEventLeftMouseDragged ) |
				                         CGEventMaskBit( kCGEventRightMouseDragged ) | CGEventMaskBit( kCGEventOtherMouseDragged ) | CGEventMaskBit( kCGEventScrollWheel );
				// At the head, so it sees the wheel before the wheel lock's tap (at the tail) takes it.
				tap_ = CGEventTapCreate( kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask, &MacBackend::tapped, this );
				if ( tap_ == nullptr )
				{
					return false;
				}
				tap_source_ = CFMachPortCreateRunLoopSource( nullptr, tap_, 0 );
				CFRunLoopAddSource( CFRunLoopGetMain(), tap_source_, kCFRunLoopCommonModes );
				CGEventTapEnable( tap_, true );
				log::info( "input: listening to the keyboard and mouse through an event tap" );
				return true;
			}

			static CGEventRef tapped( CGEventTapProxy /*proxy*/, CGEventType type, CGEventRef event, void* self )
			{
				static_cast<MacBackend*>( self )->handleEvent( type, event );
				return event;
			}

			void handleEvent( CGEventType type, CGEventRef event )
			{
				if ( type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput )
				{
					// macOS turns a tap off when it lags; whatever happened meanwhile is read from the keyboard.
					log::debug( "input: the event tap was turned off ({}); turning it on again", type == kCGEventTapDisabledByTimeout ? "timeout" : "user input" );
					CGEventTapEnable( tap_, true );
					resyncKeyboard();
					return;
				}
				last_input_.store( Clock::now().time_since_epoch().count(), std::memory_order_relaxed );
				switch ( type )
				{
					case kCGEventKeyDown:
					case kCGEventKeyUp:
					{
						if ( CGEventGetIntegerValueField( event, kCGKeyboardEventAutorepeat ) != 0 )
						{
							return;
						}
						const auto* code = keyCodeOf( static_cast<CGKeyCode>( CGEventGetIntegerValueField( event, kCGKeyboardEventKeycode ) ) );
						if ( code != nullptr && code->device == 0 )
						{
							handleKey( code->key, type == kCGEventKeyDown );
						}
						return;
					}
					case kCGEventFlagsChanged:
					{
						const auto* code = keyCodeOf( static_cast<CGKeyCode>( CGEventGetIntegerValueField( event, kCGKeyboardEventKeycode ) ) );
						if ( code == nullptr || code->device == 0 )
						{
							return;
						}
						handleKey( code->key, modifierHeld( *code, CGEventGetFlags( event ) ) );
						return;
					}
					case kCGEventLeftMouseDown:
					case kCGEventRightMouseDown:
					case kCGEventOtherMouseDown:
					case kCGEventLeftMouseUp:
					case kCGEventRightMouseUp:
					case kCGEventOtherMouseUp:
					{
						const auto number  = CGEventGetIntegerValueField( event, kCGMouseEventButtonNumber );
						const bool pressed = type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown;
						if ( const auto* button = std::ranges::find( mouse_buttons, number, &MouseButton::number ); button != mouse_buttons.end() )
						{
							handleButton( button->key, pressed, locationOf( event ) );
						}
						return;
					}
					case kCGEventScrollWheel:
						handleWheel( event );
						return;
					case kCGEventMouseMoved:
					case kCGEventLeftMouseDragged:
					case kCGEventRightMouseDragged:
					case kCGEventOtherMouseDragged:
						if ( trigger_active_ )
						{
							scheduleScan();
						}
						return;
					default:
						return;
				}
			}

			void handleKey( config::Key key, bool pressed )
			{
				down_.set( static_cast<std::size_t>( key ), pressed );
				if ( recording_ )
				{
					if ( pressed )
					{
						record( std::string( config::displayKeyName( key ) ) );
					}
					else
					{
						finishRecordingIfReleased();
					}
					return;
				}
				updateTrigger();
			}

			// Where an event happened: the pointer may have moved on by the time it is handled.
			static Point locationOf( CGEventRef event )
			{
				const CGPoint at = CGEventGetLocation( event );
				return { .x = static_cast<int>( std::floor( at.x ) ), .y = static_cast<int>( std::floor( at.y ) ) };
			}

			void handleButton( config::Key key, bool pressed, Point at )
			{
				down_.set( static_cast<std::size_t>( key ), pressed );
				if ( recording_ )
				{
					// Like on X11 and Windows, the main buttons only click; the others can be part of a trigger.
					if ( pressed && key != config::Key::MouseLeft && key != config::Key::MouseRight )
					{
						record( std::string( config::displayKeyName( key ) ) );
					}
					else if ( !pressed )
					{
						finishRecordingIfReleased();
					}
					return;
				}
				if ( pressed && events_.click_outside )
				{
					if ( !( popup_shown_ && popup_rect_.contains( at ) ) )
					{
						events_.click_outside( at );
					}
				}
				updateTrigger();
			}

			// With the trigger held, the wheel lengthens (away from you) or shortens (towards you) the looked-up text, a
			// step for every notch however fast it turns. Trackpads scroll by points: a notch for every so many.
			void handleWheel( CGEventRef event )
			{
				if ( recording_ )
				{
					return;
				}
				const bool continuous = CGEventGetIntegerValueField( event, kCGScrollWheelEventIsContinuous ) != 0;
				double     turned     = 0.0;
				if ( continuous )
				{
					// The glide after the fingers leave the trackpad is not the user turning anything.
					if ( CGEventGetIntegerValueField( event, kCGScrollWheelEventMomentumPhase ) != 0 )
					{
						return;
					}
					turned = static_cast<double>( CGEventGetIntegerValueField( event, kCGScrollWheelEventPointDeltaAxis1 ) ) / points_per_notch;
				}
				else
				{
					// The line delta grows with speed (acceleration); each event is one notch of the wheel.
					const auto lines = CGEventGetDoubleValueField( event, kCGScrollWheelEventFixedPtDeltaAxis1 );
					turned           = lines > 0.0 ? 1.0 : lines < 0.0 ? -1.0
					                                                   : 0.0;
				}
				if ( turned == 0.0 )
				{
					// Sideways only.
					return;
				}
				// Natural scrolling turns the deltas round; the wheel's own direction is what counts.
				if ( [NSEvent eventWithCGEvent:event].directionInvertedFromDevice )
				{
					turned = -turned;
				}
				wheel_ += turned;
				if ( trigger_active_ && wheel_length_ && events_.adjust_length )
				{
					while ( std::abs( wheel_ ) >= 1.0 )
					{
						const int notch = wheel_ > 0 ? 1 : -1;
						wheel_ -= notch;
						events_.adjust_length( notch );
					}
				}
				else
				{
					wheel_ = 0.0;
					// Scrolling the page under the pointer leaves the text behind: like a click outside.
					if ( popup_shown_ && events_.click_outside )
					{
						const Point at = locationOf( event );
						if ( !popup_rect_.contains( at ) )
						{
							events_.click_outside( at );
						}
					}
				}
			}

			// Keys and buttons as the keyboard and mouse have them now.
			void syncKeyboard()
			{
				for ( int key = 0; key < key_count; ++key )
				{
					down_.reset( static_cast<std::size_t>( key ) );
				}
				// The key state of a right-hand modifier is reported under the left one's code; the flags tell them apart.
				const CGEventFlags flags = CGEventSourceFlagsState( kCGEventSourceStateHIDSystemState );
				for ( const KeyCode& code : key_codes )
				{
					down_.set( static_cast<std::size_t>( code.key ), code.device != 0 ? modifierHeld( code, flags ) : CGEventSourceKeyState( kCGEventSourceStateHIDSystemState, code.code ) );
				}
				for ( const MouseButton& button : mouse_buttons )
				{
					down_.set( static_cast<std::size_t>( button.key ), CGEventSourceButtonState( kCGEventSourceStateHIDSystemState, static_cast<CGMouseButton>( button.number ) ) );
				}
			}

			void resyncKeyboard()
			{
				const auto believed = down_;
				syncKeyboard();
				if ( believed != down_ )
				{
					log::debug( "input: the keyboard was not as recorded, so the trigger is read afresh" );
					updateTrigger();
				}
			}

			// Once a second, as on Windows: the tap is created once Input Monitoring is allowed and turned on again if
			// macOS turned it off, and the keys believed held are checked against the keyboard.
			void checkInput()
			{
				input_checked_ = Clock::now();
				if ( recording_ )
				{
					return;
				}
				if ( tap_ == nullptr )
				{
					if ( createTap() )
					{
						syncKeyboard();
						updateTrigger();
					}
					return;
				}
				if ( !CGEventTapIsEnabled( tap_ ) )
				{
					if ( !input_lost_ )
					{
						input_lost_ = true;
						log::warn( "input: macOS stopped passing keyboard and mouse events on, so the trigger could not be seen; turning them on again" );
					}
					CGEventTapEnable( tap_, true );
				}
				else if ( input_lost_ )
				{
					input_lost_ = false;
					log::info( "input: keyboard and mouse events arrive again" );
				}
				resyncKeyboard();
			}

			[[nodiscard]] bool chordHeld() const
			{
				return !chord_.empty() && std::ranges::all_of( chord_, [this]( const config::KeyGroup& group ) {
					return std::ranges::any_of( group, [this]( config::Key key ) { return down_.test( static_cast<std::size_t>( key ) ); } );
				} );
			}

			// The one place Lexiglance takes input away from another application, hence the setting it waits for
			// (docs/anticheat.md): an active tap for the wheel alone, on only while the trigger is held over a popup.
			static CGEventRef wheelTapped( CGEventTapProxy /*proxy*/, CGEventType type, CGEventRef event, void* self )
			{
				auto*      backend = static_cast<MacBackend*>( self );
				const bool locked  = backend->wheel_locked_.load( std::memory_order_relaxed );
				if ( type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput )
				{
					if ( locked )
					{
						CGEventTapEnable( backend->wheel_tap_, true );
					}
					return event;
				}
				return type == kCGEventScrollWheel && locked ? nullptr : event;
			}

			void updateWheelLock()
			{
				const bool wanted = wheel_lock_ && wheel_length_ && trigger_active_ && popup_shown_;
				if ( wanted == wheel_locked_.load( std::memory_order_relaxed ) )
				{
					return;
				}
				if ( wanted && wheel_tap_ == nullptr )
				{
					// Taking events away needs the Accessibility permission; without it the window below scrolls as before.
					wheel_tap_ = CGEventTapCreate( kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionDefault, CGEventMaskBit( kCGEventScrollWheel ), &MacBackend::wheelTapped, this );
					if ( wheel_tap_ == nullptr )
					{
						return;
					}
					// A thread of its own: every scroll on the desktop waits for this tap, so a busy UI thread must not.
					wheel_thread_ = std::jthread( [tap = wheel_tap_]( const std::stop_token& stop ) {
						thread::setName( "lg-wheel" );
						CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource( nullptr, tap, 0 );
						CFRunLoopAddSource( CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes );
						while ( !stop.stop_requested() )
						{
							( void )CFRunLoopRunInMode( kCFRunLoopDefaultMode, 0.25, false );
						}
						CFRunLoopRemoveSource( CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes );
						CFRelease( source );
					} );
				}
				lockWheel( wanted );
			}

			void lockWheel( bool locked )
			{
				wheel_locked_.store( locked, std::memory_order_relaxed );
				if ( wheel_tap_ != nullptr )
				{
					CGEventTapEnable( wheel_tap_, locked );
				}
			}

			[[nodiscard]] bool sentenceHeld() const
			{
				return std::ranges::any_of( sentence_key_, [this]( config::Key key ) { return down_.test( static_cast<std::size_t>( key ) ); } );
			}

			void updateTrigger()
			{
				const bool held     = chordHeld();
				const bool sentence = held && sentenceHeld();
				if ( held && trigger_active_ && sentence != sentence_held_ )
				{
					sentence_held_    = sentence;
					sentence_pressed_ = sentence;
					if ( sentence )
					{
						scan( true );
					}
				}
				if ( held && !trigger_active_ )
				{
					trigger_active_   = true;
					sentence_held_    = sentence;
					sentence_pressed_ = sentence;
					wheel_            = 0.0;
					if ( events_.trigger_changed )
					{
						events_.trigger_changed( true );
					}
					updateWheelLock();
					scan( true );
				}
				else if ( !held && trigger_active_ )
				{
					trigger_active_   = false;
					sentence_held_    = false;
					sentence_pressed_ = false;
					scan_pending_     = false;
					updateWheelLock();
					if ( watching() )
					{
						checkSource();
					}
					if ( events_.trigger_released )
					{
						events_.trigger_released();
					}
					if ( events_.trigger_changed )
					{
						events_.trigger_changed( false );
					}
				}
			}

			void record( std::string name )
			{
				if ( !name.empty() && !std::ranges::contains( recorded_, name ) )
				{
					recorded_.push_back( std::move( name ) );
				}
			}

			void finishRecordingIfReleased()
			{
				for ( int key = 0; key < key_count; ++key )
				{
					if ( down_.test( static_cast<std::size_t>( key ) ) && std::ranges::contains( recorded_, config::displayKeyName( static_cast<config::Key>( key ) ) ) )
					{
						return;
					}
				}
				if ( !recorded_.empty() )
				{
					auto done  = std::move( recording_ );
					recording_ = nullptr;
					done( std::exchange( recorded_, {} ) );
				}
			}

			void scheduleScan()
			{
				if ( !scan_pending_ )
				{
					scan_pending_  = true;
					scan_deadline_ = std::max( Clock::now(), last_scan_ + delay_ );
					wakeBy( scan_deadline_ );
				}
			}

			void scan( bool force )
			{
				scan_pending_ = false;
				last_scan_    = Clock::now();

				const Point at = pointer();
				const int   dx = at.x - last_point_.x;
				const int   dy = at.y - last_point_.y;
				if ( !force && ( dx * dx ) + ( dy * dy ) < move_threshold_ * move_threshold_ )
				{
					return;
				}
				const auto   now     = Clock::now();
				const double elapsed = std::chrono::duration<double>( now - probe_time_ ).count();
				const double speed   = elapsed > 0.0 && elapsed < 0.25 ? std::hypot( at.x - probe_point_.x, at.y - probe_point_.y ) / elapsed : 0.0;
				const bool   flying  = !force && std::max( speed, probe_speed_ ) > fast_pointer;
				probe_point_         = at;
				probe_time_          = now;
				probe_speed_         = speed;
				if ( flying )
				{
					scan_pending_  = true;
					scan_deadline_ = now + settle_time;
					wakeBy( scan_deadline_ );
					return;
				}
				last_point_ = at;
				// The popup is rendered for the display the text is on.
				if ( NSScreen* screen = screenAt( at ) )
				{
					scale_.store( screen.backingScaleFactor, std::memory_order_relaxed );
				}
				if ( events_.scan )
				{
					events_.scan( last_point_, windowAt( last_point_ ), sentence_held_, std::exchange( sentence_pressed_, false ) );
				}
			}

			// The bundle identifier (e.g. "com.apple.TextEdit") for `scan.ocr_windows` and `ignored_windows`, else the name.
			std::string programOf( std::uint32_t pid, NSString* owner )
			{
				if ( const auto it = programs_.find( pid ); it != programs_.end() )
				{
					return it->second;
				}
				if ( programs_.size() > 128 )
				{
					programs_.clear();
				}
				NSString*   identifier = [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>( pid )].bundleIdentifier;
				NSString*   name       = identifier != nil ? identifier : owner;
				const char* utf8       = name.UTF8String;
				return programs_[pid]  = utf8 != nullptr ? std::string( utf8 ) : std::string();
			}

			// The popup follows the window its text came from: closing it, minimising it, moving it to another Space or
			// another application coming to the front closes the popup too.
			void watchSource( std::uint64_t window, bool shown = false )
			{
				source_         = window;
				source_checked_ = Clock::now();
				// Our own windows never become active, so another application in front means the user has moved on.
				foreground_ = shown || window != 0 ? NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier : 0;
			}

			[[nodiscard]] bool watching() const noexcept
			{
				return source_ != 0 || foreground_ != 0;
			}

			void checkSource()
			{
				source_checked_ = Clock::now();
				bool gone       = false;
				if ( foreground_ != 0 && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != foreground_ && !trigger_active_ )
				{
					gone = true;
				}
				if ( !gone && source_ != 0 )
				{
					@autoreleasepool
					{
						NSArray*      found  = CFBridgingRelease( CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>( source_ ) ) );
						NSDictionary* window = found.count > 0 ? found[0] : nil;
						// Minimised windows and those on another Space are not on screen.
						gone = window == nil || ![window[( __bridge NSString* )kCGWindowIsOnscreen] boolValue];
					}
				}
				if ( gone )
				{
					source_     = 0;
					foreground_ = 0;
					if ( events_.source_closed )
					{
						events_.source_closed();
					}
				}
			}

			void vacate( const Rect& rect )
			{
				const auto now = Clock::now();
				std::erase_if( vacated_, [&]( const auto& area ) { return area.second < now; } );
				if ( !rect.empty() )
				{
					vacated_.emplace_back( rect, now + std::chrono::milliseconds( 150 ) );
				}
			}

			// Screen areas painted by our own windows, for the OCR capture thread (as on Windows).
			void publishOverlays()
			{
				std::vector<Overlay> overlays;
				overlays.reserve( vacated_.size() + 4 );
				for ( const auto& [rect, until] : vacated_ )
				{
					overlays.push_back( { .rect = rect, .underneath = nullptr, .stipple = 0, .until = until } );
				}
				if ( popup_shown_ )
				{
					overlays.push_back( { .rect = popup_rect_, .underneath = nullptr, .stipple = 0 } );
				}
				const bool drawn  = highlight_shown_ && !transparent_.load( std::memory_order_relaxed );
				const bool marked = drawn && look_.shape == render::HighlightShape::Fill && marker_ && underlay_gray_;
				if ( marked )
				{
					overlays.push_back( { .rect = highlight_rect_, .underneath = underlay_gray_, .stipple = 0 } );
				}
				for ( const Rect& piece : highlight_pieces_ )
				{
					if ( !drawn || marked )
					{
						break;
					}
					const Rect r{ .x = highlight_rect_.x + piece.x, .y = highlight_rect_.y + piece.y, .width = piece.width, .height = piece.height };
					const int  t = std::max( 1, look_.thickness );
					switch ( look_.shape )
					{
						case render::HighlightShape::Fill:
							overlays.push_back( { .rect = r, .underneath = nullptr, .stipple = stipple() } );
							break;
						case render::HighlightShape::Outline:
						case render::HighlightShape::Brackets:
							for ( const Rect& side : { Rect{ .x = r.x, .y = r.y, .width = r.width, .height = t },
							                           Rect{ .x = r.x, .y = r.y + r.height - t, .width = r.width, .height = t },
							                           Rect{ .x = r.x, .y = r.y, .width = t, .height = r.height },
							                           Rect{ .x = r.x + r.width - t, .y = r.y, .width = t, .height = r.height } } )
							{
								overlays.push_back( { .rect = side, .underneath = nullptr, .stipple = 0 } );
							}
							break;
						case render::HighlightShape::Underline:
						case render::HighlightShape::DoubleUnderline:
						case render::HighlightShape::DottedUnderline:
						case render::HighlightShape::WavyUnderline:
						{
							const int lines = std::max( t, highlight_lines_ );
							overlays.push_back( { .rect = { .x = r.x, .y = r.y + r.height - lines, .width = r.width, .height = lines }, .underneath = nullptr, .stipple = 0 } );
							break;
						}
					}
				}
				const std::scoped_lock lock( overlays_mutex_ );
				overlays_ = std::move( overlays );
			}

			[[nodiscard]] int stipple() const noexcept
			{
				return highlight_color_.a < 0.3 ? 4 : 2;
			}

			void setHighlightPieces( std::span<const Rect> rects )
			{
				std::vector<Rect> areas;
				for ( const Rect& rect : rects )
				{
					if ( !rect.empty() )
					{
						const auto area = render::highlightArea( { .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height }, look_ );
						areas.push_back( { .x = area.x, .y = area.y, .width = area.width, .height = area.height } );
					}
				}
				highlight_rect_ = areas.front();
				for ( const Rect& area : areas )
				{
					highlight_rect_ = highlight_rect_.united( area );
				}
				highlight_pieces_.clear();
				for ( const Rect& area : areas )
				{
					highlight_pieces_.push_back( { .x = area.x - highlight_rect_.x, .y = area.y - highlight_rect_.y, .width = area.width, .height = area.height } );
				}
			}

			[[nodiscard]] std::vector<render::Box> pieceBoxes() const
			{
				std::vector<render::Box> boxes;
				boxes.reserve( highlight_pieces_.size() );
				for ( const Rect& piece : highlight_pieces_ )
				{
					boxes.push_back( { .x = piece.x, .y = piece.y, .width = piece.width, .height = piece.height } );
				}
				return boxes;
			}

			// The compositor setting off: the plain highlighter look of Windows and X11 (a bit for each point covered).
			std::vector<std::uint8_t> plainMask()
			{
				const int  w      = highlight_rect_.width;
				const int  h      = highlight_rect_.height;
				const int  stride = ( w + 7 ) / 8;
				const auto shaped = render::highlightMask( w, h, pieceBoxes(), look_ );
				if ( look_.shape != render::HighlightShape::Fill )
				{
					return shaped;
				}
				const render::Marker mark = render::marker( underlay_, w, h );
				marker_                   = mark.usable;
				if ( marker_ )
				{
					const auto&  c = highlight_color_;
					const auto&  g = mark.background;
					const double a = std::max( c.a, 0.45 );
					marker_color_  = { .r = ( g.r * ( 1.0 - a ) ) + ( c.r * a ), .g = ( g.g * ( 1.0 - a ) ) + ( c.g * a ), .b = ( g.b * ( 1.0 - a ) ) + ( c.b * a ), .a = 1.0 };
				}
				std::vector<std::uint8_t> bits( static_cast<std::size_t>( stride ) * static_cast<std::size_t>( h ), 0 );
				for ( int y = 0; y < h; ++y )
				{
					for ( int x = 0; x < w; ++x )
					{
						const auto byte   = ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride ) ) + static_cast<std::size_t>( x / 8 );
						const auto bit    = static_cast<std::uint8_t>( 1U << static_cast<unsigned>( x % 8 ) );
						const bool inside = ( shaped[byte] & bit ) != 0;
						const bool on     = marker_ ? mark.cover[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x )] != 0 : stippled( highlight_rect_.x + x, highlight_rect_.y + y, stipple() );
						if ( inside && on )
						{
							bits[byte] |= bit;
						}
					}
				}
				return bits;
			}

			// The screen under the highlight, a sample a point (for the automatic colour and the plain highlighter).
			void captureUnderlay( const Rect& rect )
			{
				const int                  w = rect.width;
				const int                  h = rect.height;
				std::vector<std::uint32_t> pixels( static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ), 0xFF000000U );
				if ( !screen_ && CGPreflightScreenCaptureAccess() )
				{
					screen_ = createCgScreen().value_or( nullptr );
				}
				if ( screen_ )
				{
					const Rect bounds = screen_->bounds();
					const int  x0     = std::max( rect.x, bounds.x );
					const int  y0     = std::max( rect.y, bounds.y );
					const int  x1     = std::min( rect.x + w, bounds.x + bounds.width );
					const int  y1     = std::min( rect.y + h, bounds.y + bounds.height );
					if ( x1 > x0 && y1 > y0 )
					{
						const auto rgb = screen_->read( { .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0 } );
						for ( int y = y0; y < y1 && !rgb.empty(); ++y )
						{
							for ( int x = x0; x < x1; ++x )
							{
								const std::size_t at                                                                                                        = ( ( static_cast<std::size_t>( y - y0 ) * static_cast<std::size_t>( x1 - x0 ) ) + static_cast<std::size_t>( x - x0 ) ) * 3;
								pixels[( static_cast<std::size_t>( y - rect.y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x - rect.x )] = 0xFF000000U | ( std::uint32_t{ rgb[at] } << 16U ) | ( std::uint32_t{ rgb[at + 1] } << 8U ) | rgb[at + 2];
							}
						}
					}
				}
				if ( highlight_shown_ && !underlay_.empty() )
				{
					const Rect& old = underlay_rect_;
					for ( int y = std::max( rect.y, old.y ); y < std::min( rect.y + h, old.y + old.height ); ++y )
					{
						for ( int x = std::max( rect.x, old.x ); x < std::min( rect.x + w, old.x + old.width ); ++x )
						{
							pixels[( static_cast<std::size_t>( y - rect.y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x - rect.x )] =
									underlay_[( static_cast<std::size_t>( y - old.y ) * static_cast<std::size_t>( old.width ) ) + static_cast<std::size_t>( x - old.x )];
						}
					}
				}

				auto gray = std::make_shared<std::vector<std::uint8_t>>( pixels.size() );
				for ( std::size_t i = 0; i < pixels.size(); ++i )
				{
					const std::uint32_t v = pixels[i];
					( *gray )[i]          = static_cast<std::uint8_t>( ( ( ( ( v >> 16U ) & 0xFFU ) * 299 ) + ( ( ( v >> 8U ) & 0xFFU ) * 587 ) + ( ( v & 0xFFU ) * 114 ) ) / 1000 );
				}
				underlay_      = std::move( pixels );
				underlay_rect_ = rect;
				underlay_gray_ = std::move( gray );
			}

			// Thread-safe: an event of our own makes the loop in run() look at its tasks and deadlines again.
			void wake()
			{
				if ( wake_pending_.exchange( true, std::memory_order_acq_rel ) )
				{
					return;
				}
				@autoreleasepool
				{
					NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:wake_subtype data1:0 data2:0];
					[NSApp postEvent:event atStart:NO];
				}
			}

			// From the tap's callback, which runs inside the wait: an earlier deadline than the one waited for.
			void wakeBy( Clock::time_point when )
			{
				if ( when < waiting_until_ )
				{
					wake();
				}
			}

			void runTasks()
			{
				std::vector<Task> tasks;
				{
					const std::scoped_lock lock( tasks_mutex_ );
					tasks.swap( tasks_ );
				}
				for ( auto& task : tasks )
				{
					task();
				}
			}

			void sessionEnding()
			{
				if ( session_ended_ )
				{
					return;
				}
				session_ended_ = true;
				log::info( "the session is ending" );
				if ( events_.session_ending )
				{
					events_.session_ending();
				}
				quit();
			}

			void ensurePopupWindow()
			{
				if ( popup_window_ == nil )
				{
					popup_view_         = [[LGView alloc] initWithFrame:NSMakeRect( 0, 0, 1, 1 )];
					popup_view_.onEvent = ^( NSEvent* event ) {
					  popupEvent( event );
					};
					popup_window_ = makePanel( popup_view_, true );
				}
			}

			void ensureHighlightWindow()
			{
				if ( highlight_window_ == nil )
				{
					// Click-through: every click reaches the application underneath.
					highlight_view_   = [[LGView alloc] initWithFrame:NSMakeRect( 0, 0, 1, 1 )];
					highlight_window_ = makePanel( highlight_view_, false );
				}
			}

			// Where an event is in the popup's image (pixels, y from the top of the window).
			[[nodiscard]] Point popupPixel( NSEvent* event ) const
			{
				const NSPoint at = [popup_view_ convertPoint:event.locationInWindow fromView:nil];
				return { .x = static_cast<int>( std::floor( at.x * popup_backing_ ) ), .y = static_cast<int>( std::floor( at.y * popup_backing_ ) ) };
			}

			void popupEvent( NSEvent* event )
			{
				if ( !popup_.image )
				{
					return;
				}
				const Point at = popupPixel( event );
				// Of the other buttons only the middle one does something here, as on Windows.
				const bool other = event.type == NSEventTypeOtherMouseDown || event.type == NSEventTypeOtherMouseDragged || event.type == NSEventTypeOtherMouseUp;
				if ( other && event.buttonNumber != 2 )
				{
					return;
				}
				switch ( event.type )
				{
					case NSEventTypeLeftMouseDown:
						click( at.x, at.y + scroll_ );
						paintPopup();
						return;
					case NSEventTypeRightMouseDown:
					case NSEventTypeOtherMouseDown:
						if ( other == select_middle_ )
						{
							beginSelection( at.x, at.y );
						}
						// The middle button is free unless it is the one that selects text.
						else if ( other )
						{
							playAt( at.y + scroll_ );
						}
						return;
					case NSEventTypeRightMouseDragged:
					case NSEventTypeOtherMouseDragged:
						if ( selecting_ )
						{
							extendSelection( at.x, at.y );
						}
						return;
					case NSEventTypeRightMouseUp:
					case NSEventTypeOtherMouseUp:
						if ( selecting_ && other == select_middle_ )
						{
							finishSelection();
						}
						return;
					case NSEventTypeScrollWheel:
						// With the trigger held the wheel changes the looked-up length instead (see handleWheel()).
						if ( !( trigger_active_ && wheel_length_ ) )
						{
							// Trackpads scroll by points. A slow notch of a wheel is a tenth of a line, a fast one several: each notch
							// goes as far as on Windows instead.
							const double dy  = event.scrollingDeltaY;
							const double by  = event.hasPreciseScrollingDeltas ? dy * popup_backing_ : ( dy > 0 ? 1 : dy < 0 ? -1
							                                                                                                 : 0 ) *
							                                                                                   popup_.style.px( 56 );
							const int    max = std::max( 0, popup_.image->height() - popup_size_.height );
							scroll_          = std::clamp( scroll_ - static_cast<int>( std::lround( by ) ), 0, max );
							paintPopup();
						}
						return;
					default:
						return;
				}
			}

			void copy( int content_y )
			{
				const auto* region = popup_.image ? popup_.image->regionAt( content_y ) : nullptr;
				if ( region == nullptr || region->text.empty() )
				{
					return;
				}
				if ( writePasteboard( region->text ) )
				{
					showBadge( "Copied ✓", std::chrono::milliseconds( 900 ) );
				}
			}

			void click( int x, int content_y )
			{
				select_anchor_.reset();
				select_focus_.reset();
				log::debug( "popup click at {},{} (content)", x, content_y );
				if ( const auto* button = popup_.image ? popup_.image->buttonAt( x, content_y ) : nullptr )
				{
					if ( events_.popup_action )
					{
						events_.popup_action( button->entry, button->action );
					}
					return;
				}
				copy( content_y );
			}

			void playAt( int content_y )
			{
				const auto entry = popup_.image ? popup_.image->entryAt( content_y ) : std::nullopt;
				if ( entry && events_.popup_action )
				{
					log::debug( "popup middle click on entry {}", *entry );
					events_.popup_action( *entry, render::PopupAction::Audio );
				}
			}

			void beginSelection( int x, int y )
			{
				if ( !popup_.image )
				{
					return;
				}
				selecting_     = true;
				select_press_  = { .x = x, .y = y };
				select_anchor_ = popup_.image->glyphAt( x, y + scroll_ );
				select_focus_.reset();
				paintPopup();
			}

			void extendSelection( int x, int y )
			{
				if ( !popup_.image || !select_anchor_ )
				{
					return;
				}
				const int max = std::max( 0, popup_.image->height() - popup_size_.height );
				if ( y < 0 )
				{
					scroll_ = std::max( 0, scroll_ - popup_.style.px( 12 ) );
				}
				else if ( y > popup_size_.height )
				{
					scroll_ = std::min( max, scroll_ + popup_.style.px( 12 ) );
				}
				if ( std::abs( x - select_press_.x ) + std::abs( y - select_press_.y ) >= 3 )
				{
					select_focus_ = popup_.image->glyphAt( x, y + scroll_ );
				}
				paintPopup();
			}

			void finishSelection()
			{
				selecting_ = false;
				if ( popup_.image && select_anchor_ && select_focus_ )
				{
					if ( const std::string text = popup_.image->selectedText( *select_anchor_, *select_focus_ ); !text.empty() && writePasteboard( text ) )
					{
						showBadge( "Copied ✓", std::chrono::milliseconds( 900 ) );
						return;
					}
				}
				select_anchor_.reset();
				select_focus_.reset();
				paintPopup();
			}

			bool writePasteboard( const std::string& text )
			{
				NSString* string = [NSString stringWithUTF8String:text.c_str()];
				if ( string == nil )
				{
					return false;
				}
				NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
				[pasteboard clearContents];
				const bool stored = [pasteboard setString:string forType:NSPasteboardTypeString];
				// Our own copy is not a selection to look up.
				own_pasteboard_  = pasteboard.changeCount;
				pasteboard_seen_ = own_pasteboard_;
				return stored;
			}

			void pasteboardChanged()
			{
				pasteboard_checked_   = Clock::now();
				NSPasteboard*   board = NSPasteboard.generalPasteboard;
				const NSInteger count = board.changeCount;
				if ( count == pasteboard_seen_ )
				{
					return;
				}
				pasteboard_seen_ = count;
				if ( count == own_pasteboard_ || !events_.selection )
				{
					return;
				}
				if ( selection_mode_ == config::SelectionMode::WithTrigger && !trigger_active_ )
				{
					return;
				}
				// Another program wrote it: a lone surrogate has no UTF-8 form.
				const char* text = [board stringForType:NSPasteboardTypeString].UTF8String;
				if ( text != nullptr && *text != '\0' )
				{
					events_.selection( std::string( text ), pointer() );
				}
			}

			void paintPopup( bool moved = false )
			{
				if ( ( !popup_shown_ && !moved ) || !popup_.image || popup_canvas_.surface() == nullptr )
				{
					return;
				}
				const std::string_view badge = badge_until_ != Clock::time_point{} ? std::string_view( badge_ ) : std::string_view();
				cairo_t*               cr    = cairo_create( popup_canvas_.surface() );
				cairo_set_operator( cr, CAIRO_OPERATOR_CLEAR );
				cairo_paint( cr );
				cairo_set_operator( cr, CAIRO_OPERATOR_OVER );
				render::composePopup( cr, *popup_.image, scroll_, popup_size_, popup_.style, badge );
				if ( select_anchor_ && select_focus_ )
				{
					render::drawSelection( cr, *popup_.image, *select_anchor_, *select_focus_, scroll_, popup_size_, popup_.style );
				}
				cairo_destroy( cr );
				popup_canvas_.present( popup_view_ );
			}

			// The highlight is laid out in points and drawn at the display's scale.
			void paintHighlight()
			{
				cairo_surface_t* surface = highlight_canvas_.surface();
				if ( surface == nullptr )
				{
					return;
				}
				const auto& c = highlight_color_;
				if ( transparent_.load( std::memory_order_relaxed ) )
				{
					cairo_t* cr = cairo_create( surface );
					cairo_set_operator( cr, CAIRO_OPERATOR_CLEAR );
					cairo_paint( cr );
					cairo_set_operator( cr, CAIRO_OPERATOR_OVER );
					cairo_scale( cr, highlight_scale_, highlight_scale_ );
					render::drawHighlight( cr, pieceBoxes(), look_, c );
					cairo_destroy( cr );
				}
				else
				{
					cairo_surface_flush( surface );
					const auto          bits   = plainMask();
					const int           w      = highlight_rect_.width;
					const int           h      = highlight_rect_.height;
					const int           stride = ( w + 7 ) / 8;
					const std::uint32_t colour = premultiplied( look_.shape == render::HighlightShape::Fill && marker_ ? marker_color_ : c, 1.0 );
					std::uint32_t*      pixels = highlight_canvas_.pixels();
					const int           row    = highlight_canvas_.stride();
					const int           pw     = cairo_image_surface_get_width( surface );
					const int           ph     = cairo_image_surface_get_height( surface );
					for ( int py = 0; py < ph; ++py )
					{
						const int y = std::min( h - 1, static_cast<int>( py / highlight_scale_ ) );
						for ( int px = 0; px < pw; ++px )
						{
							const int  x                                                                                                  = std::min( w - 1, static_cast<int>( px / highlight_scale_ ) );
							const auto byte                                                                                               = ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride ) ) + static_cast<std::size_t>( x / 8 );
							const bool on                                                                                                 = ( bits[byte] & ( 1U << static_cast<unsigned>( x % 8 ) ) ) != 0;
							pixels[( static_cast<std::size_t>( py ) * static_cast<std::size_t>( row ) ) + static_cast<std::size_t>( px )] = on ? colour : 0U;
						}
					}
					cairo_surface_mark_dirty( surface );
				}
				highlight_canvas_.present( highlight_view_ );
			}

			// The usable area of the display at a point (menu bar and Dock left out), in Quartz coordinates.
			static Rect monitorAt( Point point )
			{
				NSScreen* screen = screenAt( point );
				return screen != nil ? quartzRect( screen.visibleFrame ) : Rect{ .x = 0, .y = 0, .width = 1920, .height = 1080 };
			}

			// Pixels a point on the display at a point.
			static double backingAt( Point point )
			{
				NSScreen* screen = screenAt( point );
				return screen != nil ? screen.backingScaleFactor : 1.0;
			}

			// The scale of the display under the pointer and the colour mode (System Settings -> Appearance).
			void loadDesktopSettings()
			{
				if ( NSScreen* screen = screenAt( pointer() ) )
				{
					scale_.store( screen.backingScaleFactor, std::memory_order_relaxed );
				}
				dark_.store( darkAppearance(), std::memory_order_relaxed );
			}

			LGAppDelegate*                delegate_         = nil;
			id                            activity_         = nil;
			LGPanel*                      popup_window_     = nil;
			LGPanel*                      highlight_window_ = nil;
			LGView*                       popup_view_       = nil;
			LGView*                       highlight_view_   = nil;
			std::vector<id>               workspace_observers_;
			std::vector<id>               observers_;
			CFMachPortRef                 tap_        = nullptr;
			CFRunLoopSourceRef            tap_source_ = nullptr;
			CFMachPortRef                 wheel_tap_  = nullptr;
			std::atomic<bool>             wheel_locked_{ false };
			std::jthread                  wheel_thread_;
			std::unique_ptr<ScreenReader> screen_;
			Canvas                        popup_canvas_;
			Canvas                        highlight_canvas_;
			double                        highlight_scale_ = 1.0;
			double                        popup_backing_   = 1.0;
			std::vector<Rect>             highlight_pieces_;

			bool              popup_shown_ = false;
			std::uint64_t     source_      = 0;
			pid_t             foreground_  = 0;
			Clock::time_point source_checked_;
			PopupContent      popup_;
			render::PopupSize popup_size_;
			Rect              popup_rect_;
			int               scroll_         = 0;
			int               popup_offset_x_ = 0;
			int               popup_offset_y_ = 10;

			bool          highlight_shown_ = false;
			Rect          highlight_rect_;
			render::Color highlight_color_;

			NSInteger         own_pasteboard_  = 0;
			NSInteger         pasteboard_seen_ = 0;
			Clock::time_point pasteboard_checked_;
			Clock::time_point badge_until_;
			std::string       badge_;

			config::KeyChord      chord_;
			config::KeyGroup      sentence_key_;
			bool                  sentence_held_    = false;
			bool                  sentence_pressed_ = false;
			std::bitset<32>       down_;
			bool                  trigger_active_ = false;
			Clock::time_point     input_checked_;
			bool                  input_lost_     = false;
			double                wheel_          = 0.0;
			config::SelectionMode selection_mode_ = config::SelectionMode::Off;
			bool                  ocr_            = false;
			bool                  session_ended_  = false;

			std::chrono::milliseconds delay_{ 20 };
			int                       move_threshold_ = 3;
			bool                      scan_pending_   = false;
			Clock::time_point         scan_deadline_;
			Clock::time_point         waiting_until_ = Clock::time_point::max();
			Clock::time_point         last_scan_;
			Clock::time_point         probe_time_;
			Point                     probe_point_;
			double                    probe_speed_ = 0.0;
			Point                     last_point_{ .x = INT_MIN / 2, .y = INT_MIN / 2 };

			std::unordered_map<std::uint32_t, std::string> programs_;

			std::function<void( std::vector<std::string> )> recording_;
			std::vector<std::string>                        recorded_;

			Events                                                              events_;
			std::mutex                                                          tasks_mutex_;
			std::mutex                                                          overlays_mutex_;
			std::vector<Overlay>                                                overlays_;
			render::HighlightLook                                               look_;
			int                                                                 highlight_lines_ = 0;
			config::PopupPlacement                                              placement_       = config::PopupPlacement::BelowText;
			config::Compositor                                                  compositor_mode_ = config::Compositor::Auto;
			bool                                                                highlight_auto_  = false;
			bool                                                                wheel_length_    = true;
			bool                                                                wheel_lock_      = false;
			bool                                                                marker_          = false;
			render::Color                                                       marker_color_;
			bool                                                                select_middle_ = false;
			bool                                                                selecting_     = false;
			Point                                                               select_press_;
			std::optional<std::size_t>                                          select_anchor_;
			std::optional<std::size_t>                                          select_focus_;
			std::vector<std::uint32_t>                                          underlay_;
			std::vector<std::pair<Rect, std::chrono::steady_clock::time_point>> vacated_;
			Rect                                                                underlay_rect_;
			std::shared_ptr<const std::vector<std::uint8_t>>                    underlay_gray_;
			std::vector<Task>                                                   tasks_;
			std::atomic<bool>                                                   quit_{ false };
			std::atomic<bool>                                                   wake_pending_{ false };
			std::atomic<bool>                                                   transparent_{ true };
			std::atomic<double>                                                 scale_{ 1.0 };
			std::atomic<bool>                                                   dark_{ false };
			std::atomic<Clock::rep>                                             last_input_{ 0 };
		};

	} // namespace

	Result<std::unique_ptr<Backend>> createMacBackend()
	{
		auto backend = std::make_unique<MacBackend>();
		if ( auto opened = backend->open(); !opened )
		{
			return std::unexpected( opened.error() );
		}
		return backend;
	}

} // namespace lexiglance::platform
