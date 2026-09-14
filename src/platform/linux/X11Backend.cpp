#include "X11Backend.h"

#ifdef LEXIGLANCE_HAVE_ATSPI
	#include "AtspiCapture.h"
#endif
#include "../Capture.h"
#include "../ocr/OcrCapture.h"
#include "X11Screen.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cairo-xlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// Pointer speed (pixels per second at scale 1) above which nothing is looked up, and how long after slowing
		// down the pointer is looked at again.
		constexpr double fast_pointer = 1500.0;
		constexpr auto   settle_time  = std::chrono::milliseconds( 40 );

		int ignoreXErrors( Display* /*display*/, XErrorEvent* event )
		{
			// Windows vanish between queries all the time; the default handler would terminate the daemon.
			log::trace( "X error {} (request {}.{})", static_cast<int>( event->error_code ), static_cast<int>( event->request_code ), static_cast<int>( event->minor_code ) );
			return 0;
		}

		int fatalXError( Display* /*display*/ )
		{
			log::error( "lost the connection to the X server" );
			std::_Exit( 1 );
		}

		std::vector<KeySym> keysymsOf( config::Key key )
		{
			using config::Key;
			switch ( key )
			{
				case Key::ShiftL:
					return { XK_Shift_L };
				case Key::ShiftR:
					return { XK_Shift_R };
				case Key::ControlL:
					return { XK_Control_L };
				case Key::ControlR:
					return { XK_Control_R };
				case Key::AltL:
					return { XK_Alt_L, XK_Meta_L };
				case Key::AltR:
					return { XK_Alt_R, XK_Meta_R, XK_ISO_Level3_Shift };
				case Key::SuperL:
					return { XK_Super_L };
				case Key::SuperR:
					return { XK_Super_R };
				case Key::CapsLock:
					return { XK_Caps_Lock };
				case Key::Menu:
					return { XK_Menu };
				case Key::Space:
					return { XK_space };
				case Key::Tab:
					return { XK_Tab };
				case Key::Escape:
					return { XK_Escape };
				default:
					break;
			}
			if ( key >= Key::F1 && key <= Key::F12 )
			{
				return { static_cast<KeySym>( XK_F1 + ( static_cast<int>( key ) - static_cast<int>( Key::F1 ) ) ) };
			}
			return {};
		}

		int buttonOf( config::Key key )
		{
			using config::Key;
			switch ( key )
			{
				case Key::MouseLeft:
					return 1;
				case Key::MouseMiddle:
					return 2;
				case Key::MouseRight:
					return 3;
				case Key::MouseBack:
					return 8;
				case Key::MouseForward:
					return 9;
				default:
					return 0;
			}
		}

		struct Atoms
		{
			explicit Atoms( Display* display, int screen ) :
				wm_state( XInternAtom( display, "WM_STATE", False ) ),
				net_wm_pid( XInternAtom( display, "_NET_WM_PID", False ) ),
				net_wm_state( XInternAtom( display, "_NET_WM_STATE", False ) ),
				net_wm_state_fullscreen( XInternAtom( display, "_NET_WM_STATE_FULLSCREEN", False ) ),
				net_wm_window_type( XInternAtom( display, "_NET_WM_WINDOW_TYPE", False ) ),
				net_wm_window_type_popup( XInternAtom( display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False ) ),
				net_wm_window_type_tooltip( XInternAtom( display, "_NET_WM_WINDOW_TYPE_TOOLTIP", False ) ),
				net_wm_name( XInternAtom( display, "_NET_WM_NAME", False ) ),
				utf8_string( XInternAtom( display, "UTF8_STRING", False ) ),
				selection_property( XInternAtom( display, "LEXIGLANCE_SELECTION", False ) ),
				xsettings_settings( XInternAtom( display, "_XSETTINGS_SETTINGS", False ) ),
				xsettings_selection( XInternAtom( display, ( "_XSETTINGS_S" + std::to_string( screen ) ).c_str(), False ) ),
				compositor_selection( XInternAtom( display, ( "_NET_WM_CM_S" + std::to_string( screen ) ).c_str(), False ) ),
				clipboard( XInternAtom( display, "CLIPBOARD", False ) ),
				targets( XInternAtom( display, "TARGETS", False ) )
			{
			}

			Atom wm_state;
			Atom net_wm_pid;
			Atom net_wm_state;
			Atom net_wm_state_fullscreen;
			Atom net_wm_window_type;
			Atom net_wm_window_type_popup;
			Atom net_wm_window_type_tooltip;
			Atom net_wm_name;
			Atom utf8_string;
			Atom selection_property;
			Atom xsettings_settings;
			Atom xsettings_selection;
			Atom compositor_selection;
			Atom clipboard;
			Atom targets;
		};

		// Reads a window property as raw bytes; empty when absent.
		std::vector<unsigned char> readProperty( Display* display, Window window, Atom property, Atom type, int& format )
		{
			Atom           actual_type = None;
			unsigned long  count       = 0;
			unsigned long  remaining   = 0;
			unsigned char* data        = nullptr;
			format                     = 0;
			if ( XGetWindowProperty( display, window, property, 0, 1L << 20, False, type, &actual_type, &format, &count, &remaining, &data ) != Success || data == nullptr )
			{
				return {};
			}
			const std::size_t          unit = format == 32 ? sizeof( long ) : static_cast<std::size_t>( format / 8 );
			std::vector<unsigned char> bytes( data, data + ( count * unit ) );
			XFree( data );
			return bytes;
		}

		bool containsDark( std::string_view name )
		{
			std::string lower( name );
			std::ranges::transform( lower, lower.begin(), []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; } );
			return lower.contains( "dark" );
		}

		struct CairoSurfaceDeleter
		{
			void operator()( cairo_surface_t* surface ) const noexcept
			{
				cairo_surface_destroy( surface );
			}
		};
		using SurfacePtr = std::unique_ptr<cairo_surface_t, CairoSurfaceDeleter>;

		class X11Backend final : public Backend
		{
		public:
			X11Backend() = default;

			~X11Backend() override
			{
				popup_surface_.reset();
				highlight_surface_.reset();
				if ( display_ != nullptr )
				{
					XCloseDisplay( display_ );
				}
				if ( wake_fd_ >= 0 )
				{
					::close( wake_fd_ );
				}
			}

			X11Backend( const X11Backend& )            = delete;
			X11Backend& operator=( const X11Backend& ) = delete;
			X11Backend( X11Backend&& )                 = delete;
			X11Backend& operator=( X11Backend&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "x11";
			}

			Result<> open()
			{
				const char* name = std::getenv( "DISPLAY" );
				if ( name == nullptr || *name == '\0' )
				{
					return fail( "DISPLAY is not set{}", std::getenv( "WAYLAND_DISPLAY" ) != nullptr ? " (native Wayland sessions are not supported yet)" : "" );
				}
				// The OCR reader uses a second connection from the capture thread.
				XInitThreads();
				display_ = XOpenDisplay( nullptr );
				if ( display_ == nullptr )
				{
					return fail( "cannot connect to X display {}", name );
				}
				XSetErrorHandler( &ignoreXErrors );
				XSetIOErrorHandler( &fatalXError );

				screen_ = DefaultScreen( display_ );
				root_   = RootWindow( display_, screen_ );
				atoms_  = std::make_unique<Atoms>( display_, screen_ );

				wake_fd_ = ::eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
				if ( wake_fd_ < 0 )
				{
					return fail( "eventfd failed" );
				}

				int event_base = 0;
				int error_base = 0;
				if ( XQueryExtension( display_, "XInputExtension", &xi_opcode_, &event_base, &error_base ) == False )
				{
					return fail( "the X server lacks the XInput extension" );
				}
				int major = 2;
				int minor = 2;
				if ( XIQueryVersion( display_, &major, &minor ) != Success )
				{
					return fail( "XInput 2.2 is required" );
				}
				if ( XFixesQueryExtension( display_, &xfixes_event_, &error_base ) == False )
				{
					return fail( "the X server lacks the XFixes extension" );
				}

				chooseVisual();
				loadDesktopSettings();

				// Helper window: selection transfers land here. Never mapped.
				selection_window_ = XCreateSimpleWindow( display_, root_, -10, -10, 1, 1, 0, 0, 0 );
				return {};
			}

			Result<> start( Events events ) override
			{
				events_ = std::move( events );

				selectRawEvents();

				XFixesSelectSelectionInput( display_, root_, XA_PRIMARY, XFixesSetSelectionOwnerNotifyMask );
				// A compositor starting or stopping changes how our windows must be made.
				XFixesSelectSelectionInput( display_, root_, atoms_->compositor_selection, XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask );
				XSelectInput( display_, root_, StructureNotifyMask );

				rebuildKeymap();
				syncKeyboard();
				XFlush( display_ );
				return {};
			}

			void run() override
			{
				while ( !quit_.load( std::memory_order_acquire ) )
				{
					runTasks();
					while ( XPending( display_ ) > 0 )
					{
						XEvent event{};
						XNextEvent( display_, &event );
						handle( event );
					}
					XFlush( display_ );

					int timeout = -1;
					if ( scan_pending_ )
					{
						const auto wait = std::chrono::ceil<std::chrono::milliseconds>( scan_deadline_ - Clock::now() ).count();
						timeout         = static_cast<int>( std::max<long long>( 0, wait ) );
					}
					if ( badge_until_ != Clock::time_point{} )
					{
						const auto wait  = std::chrono::ceil<std::chrono::milliseconds>( badge_until_ - Clock::now() ).count();
						const int  badge = static_cast<int>( std::max<long long>( 0, wait ) );
						timeout          = timeout < 0 ? badge : std::min( timeout, badge );
					}

					std::array<pollfd, 2> fds{ { { .fd = ConnectionNumber( display_ ), .events = POLLIN, .revents = 0 }, { .fd = wake_fd_, .events = POLLIN, .revents = 0 } } };
					( void )::poll( fds.data(), fds.size(), timeout );
					if ( ( fds[1].revents & POLLIN ) != 0 )
					{
						std::uint64_t value = 0;
						( void )::read( wake_fd_, &value, sizeof( value ) );
					}
					if ( scan_pending_ && Clock::now() >= scan_deadline_ )
					{
						scan( false );
					}
					if ( badge_until_ != Clock::time_point{} && Clock::now() >= badge_until_ )
					{
						badge_until_ = {};
						paintPopup();
					}
				}
			}

			void quit() override
			{
				quit_.store( true, std::memory_order_release );
				wake();
			}

			void post( std::move_only_function<void()> task ) override
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
				delay_          = std::chrono::milliseconds( config.scan.delay_ms );
				move_threshold_ = config.scan.move_threshold;
				selection_mode_ = config.scan.selection;
				popup_offset_x_ = config.popup.offset_x;
				popup_offset_y_ = config.popup.offset_y;
				select_button_  = config.popup.select_button == config::MouseButton::Middle ? Button2 : Button3;
				highlight_auto_ = config.popup.highlight_auto;
				wheel_length_   = config.scan.wheel_length;
				if ( compositor_mode_ != config.popup.compositor )
				{
					compositor_mode_ = config.popup.compositor;
					refreshVisual();
				}
				if ( const auto look = lookFor( config.popup ); look != look_ )
				{
					look_ = look;
					hideHighlight();
				}
				placement_      = config.popup.placement;
				trigger_active_ = false;
				syncKeyboard();
				updateWheelGrab();
			}

			void showPopup( PopupContent content ) override
			{
				if ( !content.image )
				{
					return;
				}
				const Rect before = popup_mapped_ ? popup_rect_ : Rect{};
				watchSource( static_cast<Window>( content.source ) );
				popup_     = std::move( content );
				scroll_    = 0;
				selecting_ = false;
				select_anchor_.reset();
				select_focus_.reset();
				popup_size_  = render::popupSize( *popup_.image, popup_.style );
				const int  w = popup_size_.width;
				const int  h = popup_size_.height;
				const Rect a = popup_.anchor;

				const Rect monitor = monitorAt( { .x = a.x + ( a.width / 2 ), .y = a.y + ( a.height / 2 ) } );
				const int  gap     = popup_.style.px( popup_offset_y_ );
				int        x       = a.x + popup_.style.px( popup_offset_x_ );
				// The preferred side, the other one when it does not fit there, or as much as fits at the bottom.
				const int  below      = a.y + a.height + gap;
				const int  above      = a.y - gap - h;
				const bool fits_below = below + h <= monitor.y + monitor.height;
				const bool fits_above = above >= monitor.y;
				const int  last       = std::max( monitor.y, monitor.y + monitor.height - h );
				int        y          = 0;
				// The side asked for when the popup fits there, else the other one, else as low as it goes.
				const bool above_first = placement_ == config::PopupPlacement::AboveText;
				if ( above_first ? fits_above : fits_below )
				{
					y = above_first ? above : below;
				}
				else if ( above_first ? fits_below : fits_above )
				{
					y = above_first ? below : above;
				}
				else
				{
					y = last;
				}
				x = std::clamp( x, monitor.x, std::max( monitor.x, monitor.x + monitor.width - w ) );

				ensurePopupWindow();
				XMoveResizeWindow( display_, popup_window_, x, y, static_cast<unsigned>( w ), static_cast<unsigned>( h ) );
				cairo_xlib_surface_set_size( popup_surface_.get(), w, h );
				popup_rect_ = { .x = x, .y = y, .width = w, .height = h };
				if ( !popup_mapped_ )
				{
					XMapRaised( display_, popup_window_ );
					popup_mapped_ = true;
				}
				else
				{
					XRaiseWindow( display_, popup_window_ );
				}
				updateWheelGrab();
				vacate( before );
				publishOverlays();
				paintPopup();
			}

			void hidePopup() override
			{
				watchSource( 0 );
				if ( popup_mapped_ )
				{
					XUnmapWindow( display_, popup_window_ );
					popup_mapped_ = false;
					updateWheelGrab();
					popup_.image.reset();
					vacate( popup_rect_ );
					publishOverlays();
				}
			}

			[[nodiscard]] bool popupVisible() const override
			{
				return popup_mapped_;
			}

			void showHighlight( Rect rect, const render::Color& color ) override
			{
				if ( rect.empty() )
				{
					return;
				}
				const Rect before = highlight_mapped_ ? highlight_rect_ : Rect{};
				ensureHighlightWindow();
				highlight_color_ = color;
				const auto area  = render::highlightArea( { .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height }, look_ );
				highlight_rect_  = { .x = area.x, .y = area.y, .width = area.width, .height = area.height };
				// Underlines take the rows below the text (and its padding).
				highlight_lines_ = std::max( 0, area.height - rect.height - ( 2 * std::max( 0, look_.padding ) ) );
				const bool fill  = look_.shape == render::HighlightShape::Fill;
				if ( highlight_auto_ || ( fill && !transparent_ ) )
				{
					captureUnderlay( highlight_rect_ );
				}
				if ( highlight_auto_ )
				{
					highlight_color_ = render::autoHighlight( underlay_, color.a, fill );
				}
				XMoveResizeWindow(
						display_,
						highlight_window_,
						highlight_rect_.x,
						highlight_rect_.y,
						static_cast<unsigned>( highlight_rect_.width ),
						static_cast<unsigned>( highlight_rect_.height )
				);
				cairo_xlib_surface_set_size( highlight_surface_.get(), highlight_rect_.width, highlight_rect_.height );
				shapeHighlight();
				if ( !highlight_mapped_ )
				{
					XMapRaised( display_, highlight_window_ );
					highlight_mapped_ = true;
				}
				if ( popup_mapped_ )
				{
					XRaiseWindow( display_, popup_window_ );
				}
				vacate( before );
				publishOverlays();
				paintHighlight();
			}

			void showBadge( std::string text, std::chrono::milliseconds duration ) override
			{
				badge_       = std::move( text );
				badge_until_ = Clock::now() + duration;
				paintPopup();
			}

			void hideHighlight() override
			{
				if ( highlight_mapped_ )
				{
					XUnmapWindow( display_, highlight_window_ );
					highlight_mapped_ = false;
					vacate( highlight_rect_ );
					underlay_.clear();
					underlay_gray_.reset();
					marker_ = false;
					publishOverlays();
				}
			}

			WindowInfo windowAt( Point point ) override
			{
				int    x     = 0;
				int    y     = 0;
				Window child = 0;
				XTranslateCoordinates( display_, root_, root_, point.x, point.y, &x, &y, &child );
				WindowInfo window = windowInfo( child );
				window.own        = child != 0 && ( child == popup_window_ || child == highlight_window_ );
				return window;
			}

			Point pointer() override
			{
				Window       root_return  = 0;
				Window       child_return = 0;
				int          root_x       = 0;
				int          root_y       = 0;
				int          win_x        = 0;
				int          win_y        = 0;
				unsigned int state        = 0;
				XQueryPointer( display_, root_, &root_return, &child_return, &root_x, &root_y, &win_x, &win_y, &state );
				return { .x = root_x, .y = root_y };
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
				return transparent_;
			}

			[[nodiscard]] Clock::time_point lastInput() const override
			{
				return Clock::time_point( Clock::duration( last_input_.load( std::memory_order_relaxed ) ) );
			}

			void diagnose( std::vector<health::Check>& out, bool interactive ) override
			{
				// Events the server already sent count, the click that started this check among them.
				XSync( display_, False );
				while ( XPending( display_ ) > 0 )
				{
					XEvent event{};
					XNextEvent( display_, &event );
					handle( event );
				}

				// Each group of the chord needs a key the keyboard layout has (or a mouse button).
				std::string              chord;
				std::vector<std::string> missing;
				for ( const config::KeyGroup& group : chord_ )
				{
					std::string names;
					bool        found = false;
					for ( const config::Key key : group )
					{
						names.append( names.empty() ? "" : " or " ).append( config::keyName( key ) );
						const unsigned bit = 1U << static_cast<unsigned>( key );
						found              = found || config::isMouseButton( key ) || std::ranges::any_of( key_masks_, [bit]( std::uint32_t mask ) { return ( mask & bit ) != 0; } );
					}
					chord.append( chord.empty() ? "" : " + " ).append( names );
					if ( !found )
					{
						missing.push_back( std::move( names ) );
					}
				}
				health::Check trigger{ .id = "trigger", .title = "Trigger keys" };
				if ( chord_.empty() )
				{
					trigger.status = health::Severity::Error;
					trigger.detail = "No trigger is set, so nothing is ever looked up.";
					trigger.fix    = "open-scanning";
				}
				else if ( !missing.empty() )
				{
					std::string keys;
					for ( const auto& name : missing )
					{
						keys.append( keys.empty() ? "" : ", " ).append( name );
					}
					trigger.status = health::Severity::Error;
					trigger.detail = std::format( "{} is not on the current keyboard layout, so the trigger ({}) can never be held. Choose other keys.", keys, chord );
					trigger.fix    = "open-scanning";
				}
				else
				{
					trigger.detail = std::format( "Hold {} and point at a word{}.", chord, chordHeld() ? " (held right now)" : "" );
				}
				out.push_back( std::move( trigger ) );

				// Held keys follow the raw events; one release lost while the X server was busy would leave a key stuck.
				std::array<char, 32> keys{};
				XQueryKeymap( display_, keys.data() );
				std::bitset<256> actual;
				for ( std::size_t code = 0; code < 256; ++code )
				{
					actual.set( code, ( static_cast<unsigned char>( keys[code / 8] ) & ( 1U << ( code % 8 ) ) ) != 0 );
				}
				if ( actual != keys_down_ )
				{
					keys_down_ = actual;
					updateTrigger();
					out.push_back(
							{ .id     = "keyboard",
					          .title  = "Keyboard state",
					          .status = health::Severity::Info,
					          .detail = "A key was recorded as held although it was not (or the other way round); this was corrected." }
					);
				}

				health::Check input{ .id = "input", .title = "Keyboard and mouse events" };
				if ( !rawEventsSelected() )
				{
					selectRawEvents();
					XFlush( display_ );
					input.status = health::Severity::Warning;
					input.detail = "Lexiglance was no longer subscribed to keyboard and mouse events, so the trigger could not be seen; it subscribed again.";
				}
				else
				{
					const auto last = lastInput();
					const auto age  = last == Clock::time_point{} ? -1 : std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - last ).count();
					if ( age < 0 )
					{
						input.status = interactive ? health::Severity::Error : health::Severity::Info;
						input.detail = interactive ? "No keyboard or mouse event reached Lexiglance, not even the click that started this check."
						                           : "None arrived since Lexiglance started. Move the mouse or press a key, then check again.";
						input.fix    = interactive ? "restart" : "";
					}
					else if ( interactive && age > 10 )
					{
						input.status = health::Severity::Error;
						input.detail = std::format( "The click that started this check did not reach Lexiglance (the last event arrived {} s ago).", age );
						input.fix    = "restart";
					}
					else
					{
						input.detail = age < 2 ? std::string( "Arriving from the X server." ) : std::format( "Arriving from the X server (the last one {} s ago).", age );
					}
				}
				out.push_back( std::move( input ) );

				out.push_back(
						{ .id     = "desktop",
				          .title  = "Desktop",
				          .detail = std::format(
								  "X11, {}x{} pixels, scale {}x, {}.",
								  DisplayWidth( display_, screen_ ),
								  DisplayHeight( display_, screen_ ),
								  scaleFactor(),
								  transparent_ ? "compositor running (translucent overlays)" : "no compositor (plain overlays)"
						  ) }
				);
			}

			std::unique_ptr<TextCapture> createTextCapture( const config::Config& config ) override
			{
				std::unique_ptr<TextCapture> accessibility;
#ifdef LEXIGLANCE_HAVE_ATSPI
				accessibility = createAtspiCapture( config.scan.accessibility );
#endif
				std::unique_ptr<TextCapture> ocr;
				std::string                  ocr_state = "OCR off";
				if ( config.scan.ocr != config::OcrMode::Off )
				{
					auto created = createOcrCapture(
							{ .engine    = config.scan.ocr_engine,
					          .vertical  = config.scan.ocr_vertical,
					          .model     = config.scan.ocr_model,
					          .scale     = scaleFactor(),
					          .languages = lang::enabledLanguages( config.disabled_languages ),
					          .overlays  = [this] {
								  const std::scoped_lock lock( overlays_mutex_ );
								  return overlays_; },
					          .screen    = [] { return createX11Screen(); } }
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
			// Raw input of every keyboard and pointer, whichever window has the focus: observed, never grabbed.
			void selectRawEvents()
			{
				std::array<unsigned char, XIMaskLen( XI_LASTEVENT )> bits{};
				XISetMask( bits.data(), XI_RawKeyPress );
				XISetMask( bits.data(), XI_RawKeyRelease );
				XISetMask( bits.data(), XI_RawButtonPress );
				XISetMask( bits.data(), XI_RawButtonRelease );
				XISetMask( bits.data(), XI_RawMotion );
				XIEventMask mask{ .deviceid = XIAllMasterDevices, .mask_len = static_cast<int>( bits.size() ), .mask = bits.data() };
				XISelectEvents( display_, root_, &mask, 1 );
			}

			[[nodiscard]] bool rawEventsSelected()
			{
				int          count    = 0;
				XIEventMask* masks    = XIGetSelectedEvents( display_, root_, &count );
				bool         selected = false;
				for ( int i = 0; masks != nullptr && i < count; ++i )
				{
					const XIEventMask& mask = masks[i];
					selected                = selected || ( mask.deviceid == XIAllMasterDevices && mask.mask_len > ( XI_RawMotion >> 3 ) && XIMaskIsSet( mask.mask, XI_RawKeyPress ) != 0 && XIMaskIsSet( mask.mask, XI_RawMotion ) != 0 );
				}
				if ( masks != nullptr )
				{
					XFree( masks );
				}
				return selected;
			}

			// An area one of our windows has just left keeps counting as covered for OCR until the application below will
			// have painted it again.
			void vacate( const Rect& rect )
			{
				const auto now = std::chrono::steady_clock::now();
				std::erase_if( vacated_, [&]( const auto& area ) { return area.second < now; } );
				if ( !rect.empty() )
				{
					vacated_.emplace_back( rect, now + std::chrono::milliseconds( 150 ) );
				}
			}

			// Screen areas painted by our own windows, for the OCR capture thread.
			void publishOverlays()
			{
				std::vector<Overlay> overlays;
				overlays.reserve( vacated_.size() + 4 );
				for ( const auto& [rect, until] : vacated_ )
				{
					overlays.push_back( { .rect = rect, .underneath = nullptr, .stipple = 0, .until = until } );
				}
				if ( popup_mapped_ )
				{
					overlays.push_back( { .rect = popup_rect_, .underneath = nullptr, .stipple = 0 } );
				}
				if ( highlight_mapped_ && !transparent_ )
				{
					const Rect& r = highlight_rect_;
					const int   t = std::max( 1, look_.thickness );
					switch ( look_.shape )
					{
						case render::HighlightShape::Fill:
							if ( marker_ && underlay_gray_ )
							{
								overlays.push_back( { .rect = highlight_rect_, .underneath = underlay_gray_, .stipple = 0 } );
							}
							else
							{
								overlays.push_back( { .rect = highlight_rect_, .underneath = nullptr, .stipple = stipple() } );
							}
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

			// Dots on every other pixel, or on every fourth for light tints.
			[[nodiscard]] int stipple() const noexcept
			{
				return highlight_color_.a < 0.3 ? 4 : 2;
			}

			// Without a compositor the lines are cut out with the window shape, so the text under them stays visible.
			void shapeHighlight()
			{
				if ( transparent_ )
				{
					XShapeCombineMask( display_, highlight_window_, ShapeBounding, 0, 0, None, ShapeSet );
					return;
				}
				// The shape the look draws (rounded corners included), as a bitmap.
				const int  w      = highlight_rect_.width;
				const int  h      = highlight_rect_.height;
				const int  stride = ( w + 7 ) / 8;
				const auto shaped = render::highlightMask( w, h, look_ );
				if ( look_.shape == render::HighlightShape::Fill )
				{
					// A fill covers the background around the glyphs and leaves the glyphs themselves as live screen pixels,
					// like a highlighter pen. On a textured background, where glyphs cannot be told apart, it is a fine pattern
					// of dots instead, through which everything stays visible.
					const render::Marker mark = render::marker( underlay_, w, h );
					marker_                   = mark.usable;
					if ( marker_ )
					{
						const auto&  c = highlight_color_;
						const auto&  g = mark.background;
						const double a = std::max( c.a, 0.45 );
						marker_color_  = { .r = ( g.r * ( 1.0 - a ) ) + ( c.r * a ), .g = ( g.g * ( 1.0 - a ) ) + ( c.g * a ), .b = ( g.b * ( 1.0 - a ) ) + ( c.b * a ), .a = 1.0 };
					}
					std::vector<unsigned char> bits( static_cast<std::size_t>( stride ) * static_cast<std::size_t>( h ), 0 );
					for ( int y = 0; y < h; ++y )
					{
						for ( int x = 0; x < w; ++x )
						{
							const auto byte   = ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride ) ) + static_cast<std::size_t>( x / 8 );
							const auto bit    = static_cast<unsigned char>( 1U << static_cast<unsigned>( x % 8 ) );
							const bool inside = ( shaped[byte] & bit ) != 0;
							const bool on     = marker_ ? mark.cover[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x )] != 0 : stippled( highlight_rect_.x + x, highlight_rect_.y + y, stipple() );
							if ( inside && on )
							{
								bits[byte] |= bit;
							}
						}
					}
					const Pixmap mask = XCreateBitmapFromData( display_, highlight_window_, reinterpret_cast<const char*>( bits.data() ), static_cast<unsigned>( w ), static_cast<unsigned>( h ) );
					XShapeCombineMask( display_, highlight_window_, ShapeBounding, 0, 0, mask, ShapeSet );
					XFreePixmap( display_, mask );
					return;
				}
				// Lines: the window is cut to them, so the text between stays visible.
				const Pixmap mask = XCreateBitmapFromData( display_, highlight_window_, reinterpret_cast<const char*>( shaped.data() ), static_cast<unsigned>( w ), static_cast<unsigned>( h ) );
				XShapeCombineMask( display_, highlight_window_, ShapeBounding, 0, 0, mask, ShapeSet );
				XFreePixmap( display_, mask );
			}

			// Without a compositor a translucent fill is emulated: the pixels under the highlight are captured and shown
			// tinted. Where the previous highlight still covers the screen, its own capture stands in for it.
			void captureUnderlay( const Rect& rect )
			{
				const int                  w = rect.width;
				const int                  h = rect.height;
				std::vector<std::uint32_t> pixels( static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ), 0xFF000000U );
				const Rect                 screen{ .x = 0, .y = 0, .width = DisplayWidth( display_, screen_ ), .height = DisplayHeight( display_, screen_ ) };
				const int                  x0 = std::max( rect.x, screen.x );
				const int                  y0 = std::max( rect.y, screen.y );
				const int                  x1 = std::min( rect.x + w, screen.x + screen.width );
				const int                  y1 = std::min( rect.y + h, screen.y + screen.height );
				if ( x1 > x0 && y1 > y0 )
				{
					if ( XImage* image = XGetImage( display_, root_, x0, y0, static_cast<unsigned>( x1 - x0 ), static_cast<unsigned>( y1 - y0 ), AllPlanes, ZPixmap ) )
					{
						const auto shift = []( unsigned long mask ) { return mask == 0 ? 0U : static_cast<unsigned>( std::countr_zero( mask ) ); };
						const auto rs    = shift( image->red_mask );
						const auto gs    = shift( image->green_mask );
						const auto bs    = shift( image->blue_mask );
						for ( int y = y0; y < y1; ++y )
						{
							for ( int x = x0; x < x1; ++x )
							{
								const unsigned long v                                                                                                       = XGetPixel( image, x - x0, y - y0 );
								const auto          r                                                                                                       = static_cast<std::uint32_t>( ( v & image->red_mask ) >> rs ) & 0xFFU;
								const auto          g                                                                                                       = static_cast<std::uint32_t>( ( v & image->green_mask ) >> gs ) & 0xFFU;
								const auto          b                                                                                                       = static_cast<std::uint32_t>( ( v & image->blue_mask ) >> bs ) & 0xFFU;
								pixels[( static_cast<std::size_t>( y - rect.y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x - rect.x )] = 0xFF000000U | ( r << 16U ) | ( g << 8U ) | b;
							}
						}
						XDestroyImage( image );
					}
				}
				if ( highlight_mapped_ && !underlay_.empty() )
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

			void
			wake() const
			{
				const std::uint64_t one = 1;
				( void )::write( wake_fd_, &one, sizeof( one ) );
			}

			void runTasks()
			{
				std::vector<std::move_only_function<void()>> tasks;
				{
					const std::scoped_lock lock( tasks_mutex_ );
					tasks.swap( tasks_ );
				}
				for ( auto& task : tasks )
				{
					task();
				}
			}

			// ARGB windows give rounded corners and translucency, but only look right under a compositor.
			void chooseVisual()
			{
				transparent_ = wantsTransparency();
				XVisualInfo info{};
				if ( transparent_ && XMatchVisualInfo( display_, screen_, 32, TrueColor, &info ) != 0 )
				{
					visual_   = info.visual;
					depth_    = 32;
					colormap_ = XCreateColormap( display_, root_, visual_, AllocNone );
				}
				else
				{
					transparent_ = false;
					visual_      = DefaultVisual( display_, screen_ );
					depth_       = DefaultDepth( display_, screen_ );
					colormap_    = DefaultColormap( display_, screen_ );
				}
			}

			[[nodiscard]] bool wantsTransparency() const
			{
				const bool running = XGetSelectionOwner( display_, atoms_->compositor_selection ) != None;
				return compositor_mode_ == config::Compositor::On || ( compositor_mode_ == config::Compositor::Auto && running );
			}

			// A compositor started or stopped, or the setting changed: our windows are made again with the matching visual.
			void refreshVisual()
			{
				if ( wantsTransparency() == transparent_ )
				{
					return;
				}
				hidePopup();
				hideHighlight();
				popup_surface_.reset();
				highlight_surface_.reset();
				for ( Window* window : { &popup_window_, &highlight_window_ } )
				{
					if ( *window != 0 )
					{
						XDestroyWindow( display_, *window );
						*window = 0;
					}
				}
				if ( colormap_ != DefaultColormap( display_, screen_ ) )
				{
					XFreeColormap( display_, colormap_ );
				}
				chooseVisual();
				publishOverlays();
				log::info( "overlays are now {}", transparent_ ? "translucent (compositor)" : "plain (no compositor)" );
			}

			Window createOverlay( long event_mask, bool tooltip )
			{
				XSetWindowAttributes attributes{};
				attributes.override_redirect = True;
				attributes.colormap          = colormap_;
				attributes.border_pixel      = 0;
				attributes.background_pixel  = 0;
				attributes.event_mask        = event_mask;
				attributes.save_under        = True;

				const Window window = XCreateWindow(
						display_,
						root_,
						0,
						0,
						1,
						1,
						0,
						depth_,
						InputOutput,
						visual_,
						CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask | CWSaveUnder,
						&attributes
				);

				XClassHint  hint{};
				std::string res_name  = "lexiglance";
				std::string res_class = "Lexiglance";
				hint.res_name         = res_name.data();
				hint.res_class        = res_class.data();
				XSetClassHint( display_, window, &hint );

				// Never accept keyboard focus: the focused application keeps receiving every key.
				XWMHints wm_hints{};
				wm_hints.flags = InputHint;
				wm_hints.input = False;
				XSetWMHints( display_, window, &wm_hints );

				const Atom type = tooltip ? atoms_->net_wm_window_type_tooltip : atoms_->net_wm_window_type_popup;
				XChangeProperty( display_, window, atoms_->net_wm_window_type, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>( &type ), 1 );
				constexpr std::string_view title = "Lexiglance";
				XChangeProperty(
						display_,
						window,
						atoms_->net_wm_name,
						atoms_->utf8_string,
						8,
						PropModeReplace,
						reinterpret_cast<const unsigned char*>( title.data() ),
						static_cast<int>( title.size() )
				);
				return window;
			}

			// While the trigger is held over a popup the wheel changes the looked-up length, so it is kept from the
			// application under the pointer: a grab of the two wheel buttons only, released with the trigger.
			void updateWheelGrab()
			{
				const bool wanted = wheel_length_ && trigger_active_ && popup_mapped_;
				if ( wanted == wheel_grabbed_ )
				{
					return;
				}
				wheel_grabbed_ = wanted;
				for ( const unsigned button : { Button4, Button5 } )
				{
					if ( wanted )
					{
						XGrabButton( display_, button, AnyModifier, root_, False, ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None );
					}
					else
					{
						XUngrabButton( display_, button, AnyModifier, root_ );
					}
				}
				XFlush( display_ );
			}

			// The popup follows the window its text came from: closing it, minimising it or leaving its workspace closes the
			// popup too.
			void watchSource( Window window )
			{
				if ( window == source_ )
				{
					return;
				}
				if ( source_ != 0 )
				{
					XSelectInput( display_, source_, NoEventMask );
				}
				source_ = window;
				if ( source_ != 0 )
				{
					XSelectInput( display_, source_, StructureNotifyMask );
				}
			}

			void sourceClosed()
			{
				source_ = 0;
				if ( events_.source_closed )
				{
					events_.source_closed();
				}
			}

			void ensurePopupWindow()
			{
				if ( popup_window_ != 0 )
				{
					return;
				}
				popup_window_ = createOverlay( ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, false );
				popup_surface_.reset( cairo_xlib_surface_create( display_, popup_window_, visual_, 1, 1 ) );
			}

			void ensureHighlightWindow()
			{
				if ( highlight_window_ != 0 )
				{
					return;
				}
				highlight_window_ = createOverlay( ExposureMask, true );
				highlight_surface_.reset( cairo_xlib_surface_create( display_, highlight_window_, visual_, 1, 1 ) );

				// Click-through: an empty input shape lets every click reach the application underneath.
				const XserverRegion empty = XFixesCreateRegion( display_, nullptr, 0 );
				XFixesSetWindowShapeRegion( display_, highlight_window_, ShapeInput, 0, 0, empty );
				XFixesDestroyRegion( display_, empty );
			}

			// Clicking an entry copies its headword. Owning CLIPBOARD is the ordinary X11 clipboard protocol.
			void copy( int content_y )
			{
				const auto* region = popup_.image ? popup_.image->regionAt( content_y ) : nullptr;
				if ( region == nullptr || region->text.empty() )
				{
					return;
				}
				clipboard_ = region->text;
				XSetSelectionOwner( display_, atoms_->clipboard, selection_window_, CurrentTime );
				showBadge( "Copied ✓", std::chrono::milliseconds( 900 ) );
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

			// Dragging with the selection button selects popup text; releasing copies it.
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
				// Dragging past the edges scrolls.
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
					if ( std::string text = popup_.image->selectedText( *select_anchor_, *select_focus_ ); !text.empty() )
					{
						clipboard_ = std::move( text );
						XSetSelectionOwner( display_, atoms_->clipboard, selection_window_, CurrentTime );
						showBadge( "Copied ✓", std::chrono::milliseconds( 900 ) );
						return;
					}
				}
				select_anchor_.reset();
				select_focus_.reset();
				paintPopup();
			}

			void serveSelection( const XSelectionRequestEvent& request )
			{
				XSelectionEvent reply{};
				reply.type      = SelectionNotify;
				reply.display   = request.display;
				reply.requestor = request.requestor;
				reply.selection = request.selection;
				reply.target    = request.target;
				reply.time      = request.time;
				reply.property  = None;

				// Obsolete clients leave the property unset and expect the target name to be used.
				const Atom property = request.property != None ? request.property : request.target;
				if ( request.selection == atoms_->clipboard && !clipboard_.empty() )
				{
					if ( request.target == atoms_->targets )
					{
						const std::array<Atom, 3> supported{ atoms_->targets, atoms_->utf8_string, XA_STRING };
						XChangeProperty( display_, request.requestor, property, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>( supported.data() ), static_cast<int>( supported.size() ) );
						reply.property = property;
					}
					else if ( request.target == atoms_->utf8_string || request.target == XA_STRING )
					{
						XChangeProperty( display_, request.requestor, property, request.target, 8, PropModeReplace, reinterpret_cast<const unsigned char*>( clipboard_.data() ), static_cast<int>( clipboard_.size() ) );
						reply.property = property;
					}
				}

				XEvent event{};
				event.xselection = reply;
				XSendEvent( display_, request.requestor, False, NoEventMask, &event );
			}

			void paintPopup()
			{
				if ( !popup_mapped_ || !popup_.image )
				{
					return;
				}
				const std::string_view badge = badge_until_ != Clock::time_point{} ? std::string_view( badge_ ) : std::string_view();
				cairo_t*               cr    = cairo_create( popup_surface_.get() );
				cairo_push_group( cr );
				render::composePopup( cr, *popup_.image, scroll_, popup_size_, popup_.style, badge );
				if ( select_anchor_ && select_focus_ )
				{
					render::drawSelection( cr, *popup_.image, *select_anchor_, *select_focus_, scroll_, popup_size_, popup_.style );
				}
				cairo_pop_group_to_source( cr );
				cairo_set_operator( cr, CAIRO_OPERATOR_SOURCE );
				cairo_paint( cr );
				cairo_destroy( cr );
				cairo_surface_flush( popup_surface_.get() );
			}

			void paintHighlight()
			{
				if ( !highlight_mapped_ )
				{
					return;
				}
				const auto& c  = highlight_color_;
				cairo_t*    cr = cairo_create( highlight_surface_.get() );
				cairo_set_operator( cr, CAIRO_OPERATOR_SOURCE );
				if ( transparent_ )
				{
					// With a compositor the look is drawn as it is: a translucent fill, or opaque lines on nothing.
					cairo_set_source_rgba( cr, 0.0, 0.0, 0.0, 0.0 );
					cairo_paint( cr );
					cairo_set_operator( cr, CAIRO_OPERATOR_OVER );
					render::drawHighlight( cr, highlight_rect_.width, highlight_rect_.height, look_, c );
				}
				else if ( look_.shape == render::HighlightShape::Fill && marker_ )
				{
					// Without one the window shape does the drawing (see shapeHighlight()): a highlighter mark...
					cairo_set_source_rgb( cr, marker_color_.r, marker_color_.g, marker_color_.b );
					cairo_paint( cr );
				}
				else
				{
					// ... or the lines, or the dots of a fill on a textured background, in the colour.
					cairo_set_source_rgb( cr, c.r, c.g, c.b );
					cairo_paint( cr );
				}
				cairo_destroy( cr );
				cairo_surface_flush( highlight_surface_.get() );
			}

			Rect monitorAt( Point point )
			{
				Rect  best{ .x = 0, .y = 0, .width = DisplayWidth( display_, screen_ ), .height = DisplayHeight( display_, screen_ ) };
				int   count    = 0;
				auto* monitors = XRRGetMonitors( display_, root_, True, &count );
				for ( int i = 0; i < count; ++i )
				{
					const Rect rect{ .x = monitors[i].x, .y = monitors[i].y, .width = monitors[i].width, .height = monitors[i].height };
					if ( rect.contains( point ) )
					{
						best = rect;
						break;
					}
				}
				if ( monitors != nullptr )
				{
					XRRFreeMonitors( monitors );
				}
				return best;
			}

			// XSETTINGS (xsettingsd, xfsettingsd, gnome-settings-daemon) carries the GTK theme name and the font DPI.
			void loadDesktopSettings()
			{
				double dpi          = 96.0;
				double window_scale = 1.0;
				bool   dark         = false;

				if ( const char* resources = XResourceManagerString( display_ ); resources != nullptr )
				{
					const std::string_view text( resources );
					if ( const auto at = text.find( "Xft.dpi:" ); at != std::string_view::npos )
					{
						dpi = std::strtod( std::string( text.substr( at + 8, 16 ) ).c_str(), nullptr );
					}
				}

				xsettings_owner_ = XGetSelectionOwner( display_, atoms_->xsettings_selection );
				if ( xsettings_owner_ != None )
				{
					XSelectInput( display_, xsettings_owner_, PropertyChangeMask | StructureNotifyMask );
					int        format = 0;
					const auto data   = readProperty( display_, xsettings_owner_, atoms_->xsettings_settings, atoms_->xsettings_settings, format );
					parseXSettings( data, dpi, window_scale, dark );
				}
				else if ( const char* gtk_theme = std::getenv( "GTK_THEME" ); gtk_theme != nullptr )
				{
					dark = containsDark( gtk_theme );
				}

				if ( dpi < 48.0 || dpi > 480.0 )
				{
					dpi = 96.0;
				}
				// Quarter steps keep text crisp: 95 dpi must not become a blurry 0.99x.
				scale_.store( std::max( 0.5, std::round( dpi / 96.0 * window_scale * 4.0 ) / 4.0 ), std::memory_order_relaxed );
				dark_.store( dark, std::memory_order_relaxed );
			}

			static void parseXSettings( const std::vector<unsigned char>& data, double& dpi, double& window_scale, bool& dark )
			{
				if ( data.size() < 12 )
				{
					return;
				}
				const bool big_endian = data[0] != 0;
				const auto u16        = [&]( std::size_t at ) -> std::uint32_t {
                    return big_endian ? ( std::uint32_t{ data[at] } << 8U ) | data[at + 1] : ( std::uint32_t{ data[at + 1] } << 8U ) | data[at];
				};
				const auto u32 = [&]( std::size_t at ) -> std::uint32_t { return big_endian ? ( u16( at ) << 16U ) | u16( at + 2 ) : ( u16( at + 2 ) << 16U ) | u16( at ); };
				const auto pad = []( std::size_t n ) { return ( n + 3 ) & ~std::size_t{ 3 }; };

				const std::uint32_t count = u32( 8 );
				std::size_t         pos   = 12;
				for ( std::uint32_t i = 0; i < count && pos + 8 <= data.size(); ++i )
				{
					const unsigned char type        = data[pos];
					const std::size_t   name_length = u16( pos + 2 );
					if ( pos + 4 + pad( name_length ) + 4 > data.size() )
					{
						return;
					}
					const std::string name( reinterpret_cast<const char*>( data.data() + pos + 4 ), name_length );
					pos += 4 + pad( name_length ) + 4;

					if ( type == 0 && pos + 4 <= data.size() )
					{
						const auto value = static_cast<std::int32_t>( u32( pos ) );
						if ( name == "Xft/DPI" && value > 0 )
						{
							dpi = value / 1024.0;
						}
						else if ( name == "Gdk/WindowScalingFactor" && value > 0 )
						{
							window_scale = value;
						}
						pos += 4;
					}
					else if ( type == 1 && pos + 4 <= data.size() )
					{
						const std::size_t length = u32( pos );
						if ( pos + 4 + length > data.size() )
						{
							return;
						}
						const std::string_view value( reinterpret_cast<const char*>( data.data() + pos + 4 ), length );
						if ( name == "Net/ThemeName" )
						{
							dark = containsDark( value );
						}
						pos += 4 + pad( length );
					}
					else
					{
						pos += 8;
					}
				}
			}

			void rebuildKeymap()
			{
				key_masks_.fill( 0 );
				int min = 0;
				int max = 0;
				XDisplayKeycodes( display_, &min, &max );
				int     per_code = 0;
				KeySym* symbols  = XGetKeyboardMapping( display_, static_cast<KeyCode>( min ), max - min + 1, &per_code );
				if ( symbols == nullptr )
				{
					return;
				}
				for ( int code = min; code <= max && code < 256; ++code )
				{
					for ( int level = 0; level < per_code; ++level )
					{
						const KeySym symbol = symbols[( ( code - min ) * per_code ) + level];
						for ( int key = 0; key <= static_cast<int>( config::Key::F12 ); ++key )
						{
							const auto matches = keysymsOf( static_cast<config::Key>( key ) );
							if ( std::ranges::contains( matches, symbol ) )
							{
								key_masks_[static_cast<std::size_t>( code )] |= 1U << static_cast<unsigned>( key );
							}
						}
					}
				}
				XFree( symbols );
			}

			void syncKeyboard()
			{
				std::array<char, 32> keys{};
				XQueryKeymap( display_, keys.data() );
				keys_down_.reset();
				for ( std::size_t code = 0; code < 256; ++code )
				{
					if ( ( static_cast<unsigned char>( keys[code / 8] ) & ( 1U << ( code % 8 ) ) ) != 0 )
					{
						keys_down_.set( code );
					}
				}
			}

			[[nodiscard]] bool isDown( config::Key key ) const
			{
				if ( config::isMouseButton( key ) )
				{
					const int button = buttonOf( key );
					return button > 0 && buttons_down_.test( static_cast<std::size_t>( button ) );
				}
				const unsigned bit = 1U << static_cast<unsigned>( key );
				for ( std::size_t code = 0; code < 256; ++code )
				{
					if ( keys_down_.test( code ) && ( key_masks_[code] & bit ) != 0 )
					{
						return true;
					}
				}
				return false;
			}

			[[nodiscard]] bool chordHeld() const
			{
				return !chord_.empty() && std::ranges::all_of( chord_, [this]( const config::KeyGroup& group ) {
					return std::ranges::any_of( group, [this]( config::Key key ) { return isDown( key ); } );
				} );
			}

			void updateTrigger()
			{
				const bool held = chordHeld();
				if ( held && !trigger_active_ )
				{
					trigger_active_ = true;
					updateWheelGrab();
					syncKeyboard();
					if ( events_.trigger_changed )
					{
						events_.trigger_changed( true );
					}
					scan( true );
				}
				else if ( !held && trigger_active_ )
				{
					trigger_active_ = false;
					scan_pending_   = false;
					updateWheelGrab();
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

			void scheduleScan()
			{
				if ( !scan_pending_ )
				{
					scan_pending_  = true;
					scan_deadline_ = std::max( Clock::now(), last_scan_ + delay_ );
				}
			}

			void scan( bool force )
			{
				scan_pending_ = false;
				last_scan_    = Clock::now();

				Window       root_return = 0;
				Window       child       = 0;
				int          x           = 0;
				int          y           = 0;
				int          win_x       = 0;
				int          win_y       = 0;
				unsigned int state       = 0;
				XQueryPointer( display_, root_, &root_return, &child, &x, &y, &win_x, &win_y, &state );

				const int dx = x - last_point_.x;
				const int dy = y - last_point_.y;
				if ( !force && ( dx * dx ) + ( dy * dy ) < move_threshold_ * move_threshold_ )
				{
					return;
				}
				// A pointer flying across the screen is not reading anything: it is looked at again once it slows down,
				// instead of reading every spot it passes. The previous speed counts too, so turning round is not stopping.
				const auto   now     = Clock::now();
				const double elapsed = std::chrono::duration<double>( now - probe_time_ ).count();
				const double speed   = elapsed > 0.0 && elapsed < 0.25 ? std::hypot( x - probe_point_.x, y - probe_point_.y ) / elapsed : 0.0;
				const bool   flying  = !force && std::max( speed, probe_speed_ ) > fast_pointer * scaleFactor();
				probe_point_         = { .x = x, .y = y };
				probe_time_          = now;
				probe_speed_         = speed;
				if ( flying )
				{
					scan_pending_  = true;
					scan_deadline_ = now + settle_time;
					return;
				}
				last_point_ = { .x = x, .y = y };

				WindowInfo window = windowInfo( child );
				window.own        = child != 0 && ( child == popup_window_ || child == highlight_window_ );
				if ( events_.scan )
				{
					events_.scan( last_point_, window );
				}
			}

			// Finds the client window (the one carrying WM_STATE) below a top-level frame.
			Window clientOf( Window frame, int depth = 0 )
			{
				int format = 0;
				if ( !readProperty( display_, frame, atoms_->wm_state, AnyPropertyType, format ).empty() )
				{
					return frame;
				}
				if ( depth > 3 )
				{
					return 0;
				}
				Window       root_return   = 0;
				Window       parent_return = 0;
				Window*      children      = nullptr;
				unsigned int count         = 0;
				if ( XQueryTree( display_, frame, &root_return, &parent_return, &children, &count ) == 0 )
				{
					return 0;
				}
				Window found = 0;
				for ( unsigned int i = count; i-- > 0 && found == 0; )
				{
					found = clientOf( children[i], depth + 1 );
				}
				if ( children != nullptr )
				{
					XFree( children );
				}
				return found;
			}

			WindowInfo windowInfo( Window frame )
			{
				if ( frame == 0 )
				{
					return {};
				}
				if ( const auto it = window_cache_.find( frame ); it != window_cache_.end() && Clock::now() - it->second.second < std::chrono::seconds( 5 ) )
				{
					return it->second.first;
				}
				if ( window_cache_.size() > 128 )
				{
					window_cache_.clear();
				}

				WindowInfo   info;
				const Window client = clientOf( frame );
				info.id             = client != 0 ? client : frame;
				if ( client != 0 )
				{
					int  format = 0;
					auto pid    = readProperty( display_, client, atoms_->net_wm_pid, XA_CARDINAL, format );
					if ( pid.size() >= sizeof( long ) )
					{
						long value = 0;
						std::memcpy( &value, pid.data(), sizeof( long ) );
						info.pid = static_cast<std::uint32_t>( value );
					}

					XClassHint hint{};
					if ( XGetClassHint( display_, client, &hint ) != 0 )
					{
						info.wm_class = hint.res_class != nullptr ? hint.res_class : "";
						if ( hint.res_name != nullptr )
						{
							info.wm_class = std::string( hint.res_name ) + "." + info.wm_class;
							XFree( hint.res_name );
						}
						if ( hint.res_class != nullptr )
						{
							XFree( hint.res_class );
						}
					}

					const auto states = readProperty( display_, client, atoms_->net_wm_state, XA_ATOM, format );
					for ( std::size_t i = 0; i + sizeof( long ) <= states.size(); i += sizeof( long ) )
					{
						long atom = 0;
						std::memcpy( &atom, states.data() + i, sizeof( long ) );
						info.fullscreen = info.fullscreen || std::cmp_equal( atom, atoms_->net_wm_state_fullscreen );
					}
				}
				window_cache_[frame] = { info, Clock::now() };
				return info;
			}

			[[nodiscard]] std::string keyNameFor( std::size_t code ) const
			{
				for ( int key = 0; key <= static_cast<int>( config::Key::F12 ); ++key )
				{
					if ( ( key_masks_[code] & ( 1U << static_cast<unsigned>( key ) ) ) != 0 )
					{
						return std::string( config::keyName( static_cast<config::Key>( key ) ) );
					}
				}
				return {};
			}

			void record( std::string name )
			{
				if ( !name.empty() && !std::ranges::contains( recorded_, name ) )
				{
					recorded_.push_back( std::move( name ) );
				}
			}

			// Recording ends when nothing that was recorded is still held.
			void finishRecordingIfReleased()
			{
				for ( std::size_t code = 0; code < 256; ++code )
				{
					if ( keys_down_.test( code ) && std::ranges::contains( recorded_, keyNameFor( code ) ) )
					{
						return;
					}
				}
				for ( const int button : { 2, 8, 9 } )
				{
					if ( buttons_down_.test( static_cast<std::size_t>( button ) ) )
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

			[[nodiscard]] static std::string buttonName( int button )
			{
				switch ( button )
				{
					case 2:
						return "MouseMiddle";
					case 8:
						return "MouseBack";
					case 9:
						return "MouseForward";
					default:
						return {};
				}
			}

			void handleRaw( XGenericEventCookie& cookie )
			{
				last_input_.store( Clock::now().time_since_epoch().count(), std::memory_order_relaxed );
				const auto* raw = static_cast<const XIRawEvent*>( cookie.data );
				switch ( cookie.evtype )
				{
					case XI_RawKeyPress:
					case XI_RawKeyRelease:
						if ( raw->detail >= 0 && raw->detail < 256 )
						{
							keys_down_.set( static_cast<std::size_t>( raw->detail ), cookie.evtype == XI_RawKeyPress );
							if ( recording_ )
							{
								if ( cookie.evtype == XI_RawKeyPress )
								{
									record( keyNameFor( static_cast<std::size_t>( raw->detail ) ) );
								}
								else
								{
									finishRecordingIfReleased();
								}
								break;
							}
						}
						updateTrigger();
						break;
					case XI_RawButtonPress:
					case XI_RawButtonRelease:
					{
						const bool pressed = cookie.evtype == XI_RawButtonPress;
						if ( raw->detail > 0 && raw->detail < 32 )
						{
							buttons_down_.set( static_cast<std::size_t>( raw->detail ), pressed );
						}
						if ( recording_ )
						{
							if ( pressed )
							{
								record( buttonName( raw->detail ) );
							}
							else
							{
								finishRecordingIfReleased();
							}
							break;
						}
						// With the trigger held, the wheel lengthens (up) or shortens (down) the looked-up text.
						if ( pressed && trigger_active_ && wheel_length_ && ( raw->detail == 4 || raw->detail == 5 ) && events_.adjust_length )
						{
							events_.adjust_length( raw->detail == 4 ? 1 : -1 );
						}
						// Buttons 4-7 are the wheel; only real clicks dismiss the popup.
						if ( pressed && ( raw->detail <= 3 || raw->detail >= 8 ) && events_.click_outside )
						{
							const Point at = pointer();
							if ( !( popup_mapped_ && popup_rect_.contains( at ) ) )
							{
								events_.click_outside( at );
							}
						}
						updateTrigger();
						break;
					}
					case XI_RawMotion:
						if ( trigger_active_ )
						{
							scheduleScan();
						}
						break;
					default:
						break;
				}
			}

			void requestSelection( Time timestamp )
			{
				XConvertSelection( display_, XA_PRIMARY, atoms_->utf8_string, atoms_->selection_property, selection_window_, timestamp );
			}

			void receiveSelection( const XSelectionEvent& event )
			{
				if ( event.property == None )
				{
					return;
				}
				int        format = 0;
				const auto data   = readProperty( display_, selection_window_, atoms_->selection_property, AnyPropertyType, format );
				XDeleteProperty( display_, selection_window_, atoms_->selection_property );
				if ( format != 8 || data.empty() || !events_.selection )
				{
					return;
				}
				if ( selection_mode_ == config::SelectionMode::WithTrigger && !trigger_active_ )
				{
					return;
				}
				events_.selection( std::string( data.begin(), data.end() ), pointer() );
			}

			void handle( XEvent& event )
			{
				if ( event.type == GenericEvent && event.xcookie.extension == xi_opcode_ )
				{
					if ( XGetEventData( display_, &event.xcookie ) != 0 )
					{
						handleRaw( event.xcookie );
						XFreeEventData( display_, &event.xcookie );
					}
					return;
				}

				if ( event.type == xfixes_event_ + XFixesSelectionNotify )
				{
					const auto& notify = *reinterpret_cast<const XFixesSelectionNotifyEvent*>( &event );
					if ( notify.selection == atoms_->compositor_selection )
					{
						refreshVisual();
						return;
					}
					if ( notify.selection == XA_PRIMARY && notify.owner != None && selection_mode_ != config::SelectionMode::Off )
					{
						requestSelection( notify.selection_timestamp );
					}
					return;
				}

				switch ( event.type )
				{
					case Expose:
						if ( event.xexpose.count == 0 )
						{
							if ( event.xexpose.window == popup_window_ )
							{
								paintPopup();
							}
							else if ( event.xexpose.window == highlight_window_ )
							{
								paintHighlight();
							}
						}
						break;
					case ButtonPress:
						if ( event.xbutton.window == popup_window_ && popup_.image )
						{
							const int step = popup_.style.px( 56 );
							const int max  = std::max( 0, popup_.image->height() - popup_size_.height );
							if ( event.xbutton.button == Button4 )
							{
								scroll_ = std::max( 0, scroll_ - step );
							}
							else if ( event.xbutton.button == Button5 )
							{
								scroll_ = std::min( max, scroll_ + step );
							}
							else if ( event.xbutton.button == Button1 )
							{
								click( event.xbutton.x, event.xbutton.y + scroll_ );
							}
							else if ( event.xbutton.button == select_button_ )
							{
								beginSelection( event.xbutton.x, event.xbutton.y );
							}
							paintPopup();
						}
						break;
					case MotionNotify:
						if ( event.xmotion.window == popup_window_ && selecting_ )
						{
							// Only the newest position matters.
							XEvent newer{};
							while ( XCheckTypedWindowEvent( display_, popup_window_, MotionNotify, &newer ) != 0 )
							{
								event = newer;
							}
							extendSelection( event.xmotion.x, event.xmotion.y );
						}
						break;
					case ButtonRelease:
						if ( event.xbutton.window == popup_window_ && selecting_ && event.xbutton.button == select_button_ )
						{
							finishSelection();
						}
						break;
					case SelectionNotify:
						receiveSelection( event.xselection );
						break;
					case SelectionRequest:
						serveSelection( event.xselectionrequest );
						break;
					case SelectionClear:
						if ( event.xselectionclear.selection == atoms_->clipboard )
						{
							clipboard_.clear();
						}
						break;
					case PropertyNotify:
						if ( event.xproperty.window == xsettings_owner_ && event.xproperty.atom == atoms_->xsettings_settings )
						{
							loadDesktopSettings();
						}
						break;
					case MappingNotify:
						XRefreshKeyboardMapping( &event.xmapping );
						rebuildKeymap();
						break;
					case UnmapNotify:
						if ( source_ != 0 && event.xunmap.window == source_ )
						{
							sourceClosed();
						}
						break;
					case DestroyNotify:
						if ( source_ != 0 && event.xdestroywindow.window == source_ )
						{
							sourceClosed();
						}
						if ( event.xdestroywindow.window == xsettings_owner_ )
						{
							loadDesktopSettings();
						}
						window_cache_.erase( event.xdestroywindow.window );
						break;
					default:
						break;
				}
			}

			Display*               display_ = nullptr;
			int                    screen_  = 0;
			Window                 root_    = 0;
			std::unique_ptr<Atoms> atoms_;
			int                    xi_opcode_    = -1;
			int                    xfixes_event_ = 0;
			int                    wake_fd_      = -1;

			Visual*  visual_      = nullptr;
			Colormap colormap_    = 0;
			int      depth_       = 24;
			bool     transparent_ = false;

			Window            popup_window_ = 0;
			SurfacePtr        popup_surface_;
			bool              popup_mapped_ = false;
			Window            source_       = 0;
			PopupContent      popup_;
			render::PopupSize popup_size_;
			Rect              popup_rect_;
			int               scroll_         = 0;
			int               popup_offset_x_ = 0;
			int               popup_offset_y_ = 10;

			Window        highlight_window_ = 0;
			SurfacePtr    highlight_surface_;
			bool          highlight_mapped_ = false;
			Rect          highlight_rect_;
			render::Color highlight_color_;

			Window selection_window_ = 0;
			Window xsettings_owner_  = 0;

			std::string       clipboard_;
			Clock::time_point badge_until_;
			std::string       badge_;

			config::KeyChord               chord_;
			std::array<std::uint32_t, 256> key_masks_{};
			std::bitset<256>               keys_down_;
			std::bitset<32>                buttons_down_;
			bool                           trigger_active_ = false;
			config::SelectionMode          selection_mode_ = config::SelectionMode::Off;

			std::chrono::milliseconds delay_{ 20 };
			int                       move_threshold_ = 3;
			bool                      scan_pending_   = false;
			Clock::time_point         scan_deadline_;
			Clock::time_point         last_scan_;
			Clock::time_point         probe_time_;
			Point                     probe_point_;
			double                    probe_speed_ = 0.0;
			Point                     last_point_{ .x = INT_MIN / 2, .y = INT_MIN / 2 };

			std::unordered_map<Window, std::pair<WindowInfo, Clock::time_point>> window_cache_;

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
			bool                                                                wheel_grabbed_   = false;
			bool                                                                marker_          = false;
			render::Color                                                       marker_color_;
			unsigned                                                            select_button_ = Button3;
			bool                                                                selecting_     = false;
			Point                                                               select_press_;
			std::optional<std::size_t>                                          select_anchor_;
			std::optional<std::size_t>                                          select_focus_;
			std::vector<std::uint32_t>                                          underlay_;
			std::vector<std::pair<Rect, std::chrono::steady_clock::time_point>> vacated_;
			Rect                                                                underlay_rect_;
			std::shared_ptr<const std::vector<std::uint8_t>>                    underlay_gray_;
			std::vector<std::move_only_function<void()>>                        tasks_;
			std::atomic<bool>                                                   quit_{ false };
			std::atomic<double>                                                 scale_{ 1.0 };
			std::atomic<bool>                                                   dark_{ false };
			std::atomic<Clock::rep>                                             last_input_{ 0 };
		};

	} // namespace

	Result<std::unique_ptr<Backend>> createX11Backend()
	{
		auto backend = std::make_unique<X11Backend>();
		if ( auto opened = backend->open(); !opened )
		{
			return std::unexpected( opened.error() );
		}
		return backend;
	}

} // namespace lexiglance::platform
