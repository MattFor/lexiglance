#include "WindowsBackend.h"

#include "../Capture.h"
#include "../ocr/OcrCapture.h"
#include "GdiScreen.h"
#include "UiaCapture.h"
#include "Win32.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cairo.h>

// After windows.h (from Win32.h), which they need.
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// Pointer speed (pixels per second at scale 1) above which nothing is looked up, and how long after slowing
		// down the pointer is looked at again.
		constexpr double fast_pointer = 1500.0;
		constexpr auto   settle_time  = std::chrono::milliseconds( 40 );
		// How often the window the popup's text came from is checked (closed, minimised, on another virtual desktop).
		constexpr auto source_interval = std::chrono::milliseconds( 250 );

		constexpr wchar_t window_class[] = L"LexiglanceWindow";

		// DWMWA_WINDOW_CORNER_PREFERENCE and DWMWCP_DONOTROUND (Windows 11), which older SDK headers lack.
		constexpr DWORD window_corner_preference = 33;
		constexpr DWORD do_not_round             = 1;

		std::optional<config::Key> keyOf( const RAWKEYBOARD& raw )
		{
			using config::Key;
			const bool e0 = ( raw.Flags & RI_KEY_E0 ) != 0;
			switch ( raw.VKey )
			{
				case VK_SHIFT:
					// Shift with E0 is the fake one some keys send with Num Lock on, not a key the user holds.
					if ( e0 )
					{
						return std::nullopt;
					}
					return raw.MakeCode == 0x36 ? Key::ShiftR : Key::ShiftL;
				case VK_LSHIFT:
					return Key::ShiftL;
				case VK_RSHIFT:
					return Key::ShiftR;
				case VK_CONTROL:
					return e0 ? Key::ControlR : Key::ControlL;
				case VK_LCONTROL:
					return Key::ControlL;
				case VK_RCONTROL:
					return Key::ControlR;
				case VK_MENU:
					return e0 ? Key::AltR : Key::AltL;
				case VK_LMENU:
					return Key::AltL;
				case VK_RMENU:
					return Key::AltR;
				case VK_LWIN:
					return Key::SuperL;
				case VK_RWIN:
					return Key::SuperR;
				case VK_CAPITAL:
					return Key::CapsLock;
				case VK_APPS:
					return Key::Menu;
				case VK_SPACE:
					return Key::Space;
				case VK_TAB:
					return Key::Tab;
				case VK_ESCAPE:
					return Key::Escape;
				default:
					break;
			}
			if ( raw.VKey >= VK_F1 && raw.VKey <= VK_F12 )
			{
				return static_cast<Key>( static_cast<int>( Key::F1 ) + ( raw.VKey - VK_F1 ) );
			}
			return std::nullopt;
		}

		// The virtual key GetAsyncKeyState knows a key by.
		int virtualKeyOf( config::Key key )
		{
			using config::Key;
			switch ( key )
			{
				case Key::ShiftL:
					return VK_LSHIFT;
				case Key::ShiftR:
					return VK_RSHIFT;
				case Key::ControlL:
					return VK_LCONTROL;
				case Key::ControlR:
					return VK_RCONTROL;
				case Key::AltL:
					return VK_LMENU;
				case Key::AltR:
					return VK_RMENU;
				case Key::SuperL:
					return VK_LWIN;
				case Key::SuperR:
					return VK_RWIN;
				case Key::CapsLock:
					return VK_CAPITAL;
				case Key::Menu:
					return VK_APPS;
				case Key::Space:
					return VK_SPACE;
				case Key::Tab:
					return VK_TAB;
				case Key::Escape:
					return VK_ESCAPE;
				case Key::MouseLeft:
					return VK_LBUTTON;
				case Key::MouseMiddle:
					return VK_MBUTTON;
				case Key::MouseRight:
					return VK_RBUTTON;
				case Key::MouseBack:
					return VK_XBUTTON1;
				case Key::MouseForward:
					return VK_XBUTTON2;
				default:
					break;
			}
			if ( key >= Key::F1 && key <= Key::F12 )
			{
				return VK_F1 + ( static_cast<int>( key ) - static_cast<int>( Key::F1 ) );
			}
			return 0;
		}

		constexpr int key_count = static_cast<int>( config::Key::MouseForward ) + 1;

		struct MouseButton
		{
			USHORT      down;
			USHORT      up;
			config::Key key;
		};

		constexpr std::array<MouseButton, 5> mouse_buttons{ {
				{ .down = RI_MOUSE_LEFT_BUTTON_DOWN, .up = RI_MOUSE_LEFT_BUTTON_UP, .key = config::Key::MouseLeft },
				{ .down = RI_MOUSE_RIGHT_BUTTON_DOWN, .up = RI_MOUSE_RIGHT_BUTTON_UP, .key = config::Key::MouseRight },
				{ .down = RI_MOUSE_MIDDLE_BUTTON_DOWN, .up = RI_MOUSE_MIDDLE_BUTTON_UP, .key = config::Key::MouseMiddle },
				{ .down = RI_MOUSE_BUTTON_4_DOWN, .up = RI_MOUSE_BUTTON_4_UP, .key = config::Key::MouseBack },
				{ .down = RI_MOUSE_BUTTON_5_DOWN, .up = RI_MOUSE_BUTTON_5_UP, .key = config::Key::MouseForward },
		} };

		std::uint64_t idOf( HWND window ) noexcept
		{
			return static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( window ) );
		}

		HWND windowOf( std::uint64_t id ) noexcept
		{
			return reinterpret_cast<HWND>( static_cast<std::uintptr_t>( id ) );
		}

		// The program a window belongs to, without ".exe".
		std::string programOf( DWORD pid )
		{
			HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
			if ( process == nullptr )
			{
				return {};
			}
			std::wstring path( 32768, L'\0' );
			auto         size = static_cast<DWORD>( path.size() );
			const BOOL   ok   = QueryFullProcessImageNameW( process, 0, path.data(), &size );
			CloseHandle( process );
			if ( ok == FALSE )
			{
				return {};
			}
			path.resize( size );
			return win32::narrow( std::filesystem::path( path ).stem().wstring() );
		}

		std::string classOf( HWND window )
		{
			std::array<wchar_t, 256> name{};
			const int                length = GetClassNameW( window, name.data(), static_cast<int>( name.size() ) );
			return win32::narrow( std::wstring_view( name.data(), static_cast<std::size_t>( std::max( 0, length ) ) ) );
		}

		// Apps (not the taskbar) in dark mode, as Windows' settings put it.
		bool appsUseDarkTheme()
		{
			DWORD value = 1;
			DWORD size  = sizeof( value );
			if ( RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size ) != ERROR_SUCCESS )
			{
				return false;
			}
			return value == 0;
		}

		// A 32-bit premultiplied canvas for a layered window: Cairo draws into it and UpdateLayeredWindow shows it with its
		// per-pixel alpha (rounded corners, translucency).
		class LayeredCanvas
		{
		public:
			LayeredCanvas() :
				dc_( CreateCompatibleDC( nullptr ) )
			{
			}

			~LayeredCanvas()
			{
				release();
				DeleteDC( dc_ );
			}

			LayeredCanvas( const LayeredCanvas& )            = delete;
			LayeredCanvas& operator=( const LayeredCanvas& ) = delete;
			LayeredCanvas( LayeredCanvas&& )                 = delete;
			LayeredCanvas& operator=( LayeredCanvas&& )      = delete;

			// The canvas at width x height (its old content is undefined when the size changed).
			cairo_surface_t* resize( int width, int height )
			{
				width  = std::max( 1, width );
				height = std::max( 1, height );
				if ( surface_ != nullptr && width == width_ && height == height_ )
				{
					return surface_;
				}
				release();
				BITMAPINFO info{};
				info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
				info.bmiHeader.biWidth       = width;
				info.bmiHeader.biHeight      = -height;
				info.bmiHeader.biPlanes      = 1;
				info.bmiHeader.biBitCount    = 32;
				info.bmiHeader.biCompression = BI_RGB;
				void* bits                   = nullptr;
				bitmap_                      = CreateDIBSection( dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0 );
				if ( bitmap_ == nullptr || bits == nullptr )
				{
					bitmap_ = nullptr;
					return nullptr;
				}
				previous_ = SelectObject( dc_, bitmap_ );
				bits_     = static_cast<std::uint32_t*>( bits );
				width_    = width;
				height_   = height;
				// The DIB's BGRA bytes are Cairo's ARGB32 words on a little-endian machine.
				surface_ = cairo_image_surface_create_for_data( static_cast<unsigned char*>( bits ), CAIRO_FORMAT_ARGB32, width, height, width * 4 );
				return surface_;
			}

			[[nodiscard]] cairo_surface_t* surface() const noexcept
			{
				return surface_;
			}

			// Premultiplied 0xAARRGGBB pixels, row by row, for drawing without Cairo.
			[[nodiscard]] std::uint32_t* pixels() const noexcept
			{
				return bits_;
			}

			void present( HWND window, int x, int y ) const
			{
				if ( surface_ == nullptr )
				{
					return;
				}
				cairo_surface_flush( surface_ );
				POINT         position{ .x = x, .y = y };
				SIZE          size{ .cx = width_, .cy = height_ };
				POINT         origin{ .x = 0, .y = 0 };
				BLENDFUNCTION blend{ .BlendOp = AC_SRC_OVER, .BlendFlags = 0, .SourceConstantAlpha = 255, .AlphaFormat = AC_SRC_ALPHA };
				UpdateLayeredWindow( window, nullptr, &position, &size, dc_, &origin, 0, &blend, ULW_ALPHA );
			}

		private:
			void release() noexcept
			{
				if ( surface_ != nullptr )
				{
					cairo_surface_destroy( surface_ );
					surface_ = nullptr;
				}
				if ( bitmap_ != nullptr )
				{
					SelectObject( dc_, previous_ );
					DeleteObject( bitmap_ );
					bitmap_ = nullptr;
					bits_   = nullptr;
				}
			}

			HDC              dc_;
			HBITMAP          bitmap_   = nullptr;
			HGDIOBJ          previous_ = nullptr;
			cairo_surface_t* surface_  = nullptr;
			std::uint32_t*   bits_     = nullptr;
			int              width_    = 0;
			int              height_   = 0;
		};

		std::uint32_t premultiplied( const render::Color& color, double alpha ) noexcept
		{
			const auto channel = [&]( double value ) { return static_cast<std::uint32_t>( std::lround( std::clamp( value * alpha, 0.0, 1.0 ) * 255.0 ) ); };
			return ( channel( 1.0 ) << 24U ) | ( channel( color.r ) << 16U ) | ( channel( color.g ) << 8U ) | channel( color.b );
		}

		class WindowsBackend final : public Backend
		{
		public:
			WindowsBackend() = default;

			~WindowsBackend() override
			{
				if ( clipboard_listening_ )
				{
					RemoveClipboardFormatListener( host_ );
				}
				for ( HWND* window : { &popup_window_, &highlight_window_, &host_ } )
				{
					if ( *window != nullptr )
					{
						DestroyWindow( *window );
						*window = nullptr;
					}
				}
				if ( wake_event_ != nullptr )
				{
					CloseHandle( wake_event_ );
				}
			}

			WindowsBackend( const WindowsBackend& )            = delete;
			WindowsBackend& operator=( const WindowsBackend& ) = delete;
			WindowsBackend( WindowsBackend&& )                 = delete;
			WindowsBackend& operator=( WindowsBackend&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "windows";
			}

			Result<> open()
			{
				// Physical pixels everywhere (pointer, UI Automation, screen reads, our windows), on every monitor.
				( void )SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );

				wake_event_ = CreateEventW( nullptr, FALSE, FALSE, nullptr );
				if ( wake_event_ == nullptr )
				{
					return fail( "CreateEvent failed (error {})", GetLastError() );
				}

				WNDCLASSEXW type{};
				type.cbSize        = sizeof( type );
				type.lpfnWndProc   = &WindowsBackend::procedure;
				type.hInstance     = GetModuleHandleW( nullptr );
				type.hCursor       = LoadCursorW( nullptr, MAKEINTRESOURCEW( 32512 ) ); // IDC_ARROW
				type.lpszClassName = window_class;
				if ( RegisterClassExW( &type ) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
				{
					return fail( "cannot register the window class (error {})", GetLastError() );
				}
				// Never shown: raw input, the clipboard and desktop setting changes arrive here (a message-only window would
				// not get the broadcasts).
				host_ = CreateWindowExW( WS_EX_TOOLWINDOW, window_class, L"Lexiglance", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, this );
				if ( host_ == nullptr )
				{
					return fail( "cannot create a window (error {})", GetLastError() );
				}
				screen_ = createGdiScreen().value_or( nullptr );
				loadDesktopSettings();
				return {};
			}

			Result<> start( Events events ) override
			{
				events_ = std::move( events );
				if ( !registerRawInput() )
				{
					return fail( "cannot receive keyboard and mouse input (RegisterRawInputDevices failed, error {})", GetLastError() );
				}
				syncKeyboard();
				return {};
			}

			void run() override
			{
				while ( !quit_.load( std::memory_order_acquire ) )
				{
					runTasks();
					MSG message{};
					while ( PeekMessageW( &message, nullptr, 0, 0, PM_REMOVE ) != FALSE )
					{
						if ( message.message == WM_QUIT )
						{
							quit_.store( true, std::memory_order_release );
							break;
						}
						DispatchMessageW( &message );
					}
					if ( quit_.load( std::memory_order_acquire ) )
					{
						break;
					}

					const auto now     = Clock::now();
					DWORD      timeout = INFINITE;
					const auto until   = [&]( Clock::time_point when ) {
                        const auto wait = std::chrono::ceil<std::chrono::milliseconds>( when - now ).count();
                        timeout         = std::min( timeout, static_cast<DWORD>( std::clamp<long long>( wait, 0, 0x7FFFFFFF ) ) );
					};
					if ( scan_pending_ )
					{
						until( scan_deadline_ );
					}
					if ( badge_until_ != Clock::time_point{} )
					{
						until( badge_until_ );
					}
					if ( source_ != nullptr )
					{
						until( source_checked_ + source_interval );
					}
					( void )MsgWaitForMultipleObjectsEx( 1, &wake_event_, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE );

					if ( scan_pending_ && Clock::now() >= scan_deadline_ )
					{
						scan( false );
					}
					if ( badge_until_ != Clock::time_point{} && Clock::now() >= badge_until_ )
					{
						badge_until_ = {};
						paintPopup();
					}
					if ( source_ != nullptr && Clock::now() >= source_checked_ + source_interval )
					{
						checkSource();
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
				select_middle_  = config.popup.select_button == config::MouseButton::Middle;
				highlight_auto_ = config.popup.highlight_auto;
				wheel_length_   = config.scan.wheel_length;
				if ( compositor_mode_ != config.popup.compositor )
				{
					compositor_mode_ = config.popup.compositor;
					hidePopup();
					hideHighlight();
					// Windows always composites; only "off" asks for the plain look of a desktop without a compositor.
					transparent_.store( compositor_mode_ != config::Compositor::Off, std::memory_order_relaxed );
					publishOverlays();
				}
				if ( const auto look = lookFor( config.popup ); look != look_ )
				{
					look_ = look;
					hideHighlight();
				}
				placement_ = config.popup.placement;

				// Copied text stands in for X11's selection: text hookers copy what they read to the clipboard.
				const bool listen = selection_mode_ != config::SelectionMode::Off;
				if ( listen != clipboard_listening_ )
				{
					clipboard_listening_ = listen && AddClipboardFormatListener( host_ ) != FALSE;
					if ( !listen )
					{
						RemoveClipboardFormatListener( host_ );
					}
				}
				trigger_active_ = false;
				syncKeyboard();
			}

			void showPopup( PopupContent content ) override
			{
				if ( !content.image )
				{
					return;
				}
				const Rect before = popup_shown_ ? popup_rect_ : Rect{};
				watchSource( windowOf( content.source ) );
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
				x = std::clamp( x, monitor.x, std::max( monitor.x, monitor.x + monitor.width - w ) );

				ensurePopupWindow();
				popup_rect_ = { .x = x, .y = y, .width = w, .height = h };
				popup_canvas_.resize( w, h );
				paintPopup( true );
				raise( popup_window_ );
				popup_shown_ = true;
				vacate( before );
				publishOverlays();
			}

			void hidePopup() override
			{
				watchSource( nullptr );
				if ( popup_shown_ )
				{
					ShowWindow( popup_window_, SW_HIDE );
					popup_shown_ = false;
					selecting_   = false;
					if ( GetCapture() == popup_window_ )
					{
						ReleaseCapture();
					}
					popup_.image.reset();
					vacate( popup_rect_ );
					publishOverlays();
				}
			}

			[[nodiscard]] bool popupVisible() const override
			{
				return popup_shown_;
			}

			void showHighlight( Rect rect, const render::Color& color ) override
			{
				if ( rect.empty() )
				{
					return;
				}
				const Rect before = highlight_shown_ ? highlight_rect_ : Rect{};
				ensureHighlightWindow();
				highlight_color_ = color;
				const auto area  = render::highlightArea( { .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height }, look_ );
				highlight_rect_  = { .x = area.x, .y = area.y, .width = area.width, .height = area.height };
				// Underlines take the rows below the text (and its padding).
				highlight_lines_ = std::max( 0, area.height - rect.height - ( 2 * std::max( 0, look_.padding ) ) );
				const bool fill  = look_.shape == render::HighlightShape::Fill;
				if ( highlight_auto_ || ( fill && !transparent_.load( std::memory_order_relaxed ) ) )
				{
					captureUnderlay( highlight_rect_ );
				}
				if ( highlight_auto_ )
				{
					highlight_color_ = render::autoHighlight( underlay_, color.a, fill );
				}
				highlight_canvas_.resize( highlight_rect_.width, highlight_rect_.height );
				paintHighlight();
				raise( highlight_window_ );
				highlight_shown_ = true;
				if ( popup_shown_ )
				{
					raise( popup_window_ );
				}
				vacate( before );
				publishOverlays();
			}

			void hideHighlight() override
			{
				if ( highlight_shown_ )
				{
					ShowWindow( highlight_window_, SW_HIDE );
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
				POINT at{};
				GetCursorPos( &at );
				return { .x = at.x, .y = at.y };
			}

			WindowInfo windowAt( Point point ) override
			{
				HWND       child  = WindowFromPoint( POINT{ .x = point.x, .y = point.y } );
				HWND       root   = child != nullptr ? GetAncestor( child, GA_ROOT ) : nullptr;
				WindowInfo window = windowInfo( root );
				window.own        = root != nullptr && ( root == popup_window_ || root == highlight_window_ );
				return window;
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
				// Input that already arrived counts, the click that started this check among it.
				MSG message{};
				while ( PeekMessageW( &message, host_, WM_INPUT, WM_INPUT, PM_REMOVE ) != FALSE )
				{
					DispatchMessageW( &message );
				}

				std::string chord;
				for ( const config::KeyGroup& group : chord_ )
				{
					std::string names;
					for ( const config::Key key : group )
					{
						names.append( names.empty() ? "" : " or " ).append( config::keyName( key ) );
					}
					chord.append( chord.empty() ? "" : " + " ).append( names );
				}
				health::Check trigger{ .id = "trigger", .title = "Trigger keys" };
				if ( chord_.empty() )
				{
					trigger.status = health::Severity::Error;
					trigger.detail = "No trigger is set, so nothing is ever looked up.";
					trigger.fix    = "open-scanning";
				}
				else
				{
					trigger.detail = std::format( "Hold {} and point at a word{}.", chord, chordHeld() ? " (held right now)" : "" );
				}
				out.push_back( std::move( trigger ) );

				// Held keys follow the raw input; a release lost while another desktop (a UAC prompt) was active would leave
				// a key stuck.
				const auto recorded = down_;
				syncKeyboard();
				if ( recorded != down_ )
				{
					updateTrigger();
					out.push_back(
							{ .id     = "keyboard",
					          .title  = "Keyboard state",
					          .status = health::Severity::Info,
					          .detail = "A key was recorded as held although it was not (or the other way round); this was corrected." }
					);
				}

				health::Check input{ .id = "input", .title = "Keyboard and mouse events" };
				if ( !rawInputRegistered() )
				{
					( void )registerRawInput();
					input.status = health::Severity::Warning;
					input.detail = "Lexiglance was no longer receiving keyboard and mouse input, so the trigger could not be seen; it registered again.";
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
						input.detail = age < 2 ? std::string( "Arriving through Raw Input." ) : std::format( "Arriving through Raw Input (the last one {} s ago).", age );
					}
				}
				out.push_back( std::move( input ) );

				out.push_back(
						{ .id     = "desktop",
				          .title  = "Desktop",
				          .detail = std::format(
								  "Windows, {}x{} pixels, scale {}x, {}.",
								  GetSystemMetrics( SM_CXVIRTUALSCREEN ),
								  GetSystemMetrics( SM_CYVIRTUALSCREEN ),
								  scaleFactor(),
								  supportsTransparency() ? "translucent overlays" : "plain overlays (compositor setting off)"
						  ) }
				);
			}

			std::unique_ptr<TextCapture> createTextCapture( const config::Config& config ) override
			{
				std::unique_ptr<TextCapture> accessibility = createUiaCapture();
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
					          .screen    = [] { return createGdiScreen(); } }
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
			static LRESULT CALLBACK procedure( HWND window, UINT message, WPARAM wparam, LPARAM lparam )
			{
				if ( message == WM_NCCREATE )
				{
					const auto* create = reinterpret_cast<const CREATESTRUCTW*>( lparam );
					SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( create->lpCreateParams ) );
				}
				auto* self = reinterpret_cast<WindowsBackend*>( GetWindowLongPtrW( window, GWLP_USERDATA ) );
				if ( self != nullptr )
				{
					if ( const auto handled = self->handle( window, message, wparam, lparam ) )
					{
						return *handled;
					}
				}
				return DefWindowProcW( window, message, wparam, lparam );
			}

			std::optional<LRESULT> handle( HWND window, UINT message, WPARAM wparam, LPARAM lparam )
			{
				if ( window == host_ )
				{
					return handleHost( message, wparam, lparam );
				}
				if ( window == popup_window_ )
				{
					return handlePopup( message, wparam, lparam );
				}
				if ( window == highlight_window_ && message == WM_MOUSEACTIVATE )
				{
					return MA_NOACTIVATE;
				}
				return std::nullopt;
			}

			std::optional<LRESULT> handleHost( UINT message, WPARAM wparam, LPARAM lparam )
			{
				switch ( message )
				{
					case WM_INPUT:
						handleRaw( reinterpret_cast<HRAWINPUT>( lparam ) );
						// The system cleans up after WM_INPUT in DefWindowProc.
						return std::nullopt;
					case WM_CLIPBOARDUPDATE:
						clipboardChanged();
						return 0;
					case WM_SETTINGCHANGE:
					case WM_DISPLAYCHANGE:
					case WM_DPICHANGED:
						loadDesktopSettings();
						return std::nullopt;
					case WM_QUERYENDSESSION:
						return TRUE;
					case WM_ENDSESSION:
						if ( wparam != FALSE )
						{
							// Logging off or shutting down: the daemon ends like on SIGTERM.
							quit_.store( true, std::memory_order_release );
						}
						return 0;
					default:
						return std::nullopt;
				}
			}

			std::optional<LRESULT> handlePopup( UINT message, WPARAM wparam, LPARAM lparam )
			{
				const int x = GET_X_LPARAM( lparam );
				const int y = GET_Y_LPARAM( lparam );
				switch ( message )
				{
					case WM_MOUSEACTIVATE:
						// Never take the focus: the application below keeps every key.
						return MA_NOACTIVATE;
					case WM_LBUTTONDOWN:
						if ( popup_.image )
						{
							click( x, y + scroll_ );
							paintPopup();
						}
						return 0;
					case WM_RBUTTONDOWN:
					case WM_MBUTTONDOWN:
						if ( popup_.image && ( message == WM_MBUTTONDOWN ) == select_middle_ )
						{
							SetCapture( popup_window_ );
							beginSelection( x, y );
						}
						return 0;
					case WM_MOUSEMOVE:
						if ( selecting_ )
						{
							extendSelection( x, y );
						}
						return 0;
					case WM_RBUTTONUP:
					case WM_MBUTTONUP:
						if ( selecting_ && ( message == WM_MBUTTONUP ) == select_middle_ )
						{
							ReleaseCapture();
							finishSelection();
						}
						return 0;
					case WM_CAPTURECHANGED:
						selecting_ = false;
						return 0;
					case WM_MOUSEWHEEL:
						// With the trigger held the wheel changes the looked-up length instead (see handleRaw()).
						if ( popup_.image && !( trigger_active_ && wheel_length_ ) )
						{
							const int step = popup_.style.px( 56 );
							const int max  = std::max( 0, popup_.image->height() - popup_size_.height );
							scroll_        = GET_WHEEL_DELTA_WPARAM( wparam ) > 0 ? std::max( 0, scroll_ - step ) : std::min( max, scroll_ + step );
							paintPopup();
						}
						return 0;
					default:
						return std::nullopt;
				}
			}

			// Raw input of every keyboard and mouse, whichever window has the focus: observed, never grabbed or blocked.
			bool registerRawInput()
			{
				const std::array<RAWINPUTDEVICE, 2> devices{ {
						{ .usUsagePage = 0x01, .usUsage = 0x06, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = host_ },
						{ .usUsagePage = 0x01, .usUsage = 0x02, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = host_ },
				} };
				return RegisterRawInputDevices( devices.data(), static_cast<UINT>( devices.size() ), sizeof( RAWINPUTDEVICE ) ) != FALSE;
			}

			[[nodiscard]] bool rawInputRegistered() const
			{
				std::array<RAWINPUTDEVICE, 8> devices{};
				auto                          count = static_cast<UINT>( devices.size() );
				const UINT                    found = GetRegisteredRawInputDevices( devices.data(), &count, sizeof( RAWINPUTDEVICE ) );
				if ( found == static_cast<UINT>( -1 ) )
				{
					return false;
				}
				int ours = 0;
				for ( UINT i = 0; i < found; ++i )
				{
					ours += devices[i].hwndTarget == host_ && ( devices[i].dwFlags & RIDEV_INPUTSINK ) != 0 ? 1 : 0;
				}
				return ours == 2;
			}

			void handleRaw( HRAWINPUT handle )
			{
				alignas( RAWINPUT ) std::array<std::byte, sizeof( RAWINPUT )> buffer{};
				UINT                                                          size = sizeof( buffer );
				if ( GetRawInputData( handle, RID_INPUT, buffer.data(), &size, sizeof( RAWINPUTHEADER ) ) == static_cast<UINT>( -1 ) )
				{
					return;
				}
				const auto* input = reinterpret_cast<const RAWINPUT*>( buffer.data() );
				last_input_.store( Clock::now().time_since_epoch().count(), std::memory_order_relaxed );
				if ( input->header.dwType == RIM_TYPEKEYBOARD )
				{
					handleKey( input->data.keyboard );
				}
				else if ( input->header.dwType == RIM_TYPEMOUSE )
				{
					handleMouse( input->data.mouse );
				}
			}

			void handleKey( const RAWKEYBOARD& raw )
			{
				const auto key = keyOf( raw );
				if ( !key )
				{
					return;
				}
				const bool pressed = ( raw.Flags & RI_KEY_BREAK ) == 0;
				down_.set( static_cast<std::size_t>( *key ), pressed );
				if ( recording_ )
				{
					if ( pressed )
					{
						record( std::string( config::keyName( *key ) ) );
					}
					else
					{
						finishRecordingIfReleased();
					}
					return;
				}
				updateTrigger();
			}

			void handleMouse( const RAWMOUSE& raw )
			{
				const USHORT flags = raw.usButtonFlags;
				for ( const MouseButton& button : mouse_buttons )
				{
					const bool pressed  = ( flags & button.down ) != 0;
					const bool released = ( flags & button.up ) != 0;
					if ( !pressed && !released )
					{
						continue;
					}
					down_.set( static_cast<std::size_t>( button.key ), pressed );
					if ( recording_ )
					{
						// Like on X11, the main buttons only click; the others can be part of a trigger.
						if ( pressed && button.key != config::Key::MouseLeft && button.key != config::Key::MouseRight )
						{
							record( std::string( config::keyName( button.key ) ) );
						}
						else if ( released )
						{
							finishRecordingIfReleased();
						}
						continue;
					}
					if ( pressed && events_.click_outside )
					{
						const Point at = pointer();
						if ( !( popup_shown_ && popup_rect_.contains( at ) ) )
						{
							events_.click_outside( at );
						}
					}
					updateTrigger();
				}

				// With the trigger held, the wheel lengthens (up) or shortens (down) the looked-up text. Precision touchpads
				// send fractions of a notch.
				if ( ( flags & RI_MOUSE_WHEEL ) != 0 && !recording_ )
				{
					wheel_ += static_cast<SHORT>( raw.usButtonData );
					if ( trigger_active_ && wheel_length_ && events_.adjust_length )
					{
						while ( std::abs( wheel_ ) >= WHEEL_DELTA )
						{
							const int notch = wheel_ > 0 ? 1 : -1;
							wheel_ -= notch * WHEEL_DELTA;
							events_.adjust_length( notch );
						}
					}
					else
					{
						wheel_ = 0;
					}
				}

				const bool moved = raw.lLastX != 0 || raw.lLastY != 0 || ( raw.usFlags & MOUSE_MOVE_ABSOLUTE ) != 0;
				if ( moved && trigger_active_ )
				{
					scheduleScan();
				}
			}

			// Keys as the keyboard has them now.
			void syncKeyboard()
			{
				for ( int key = 0; key < key_count; ++key )
				{
					const int code = virtualKeyOf( static_cast<config::Key>( key ) );
					down_.set( static_cast<std::size_t>( key ), code != 0 && ( GetAsyncKeyState( code ) & 0x8000 ) != 0 );
				}
			}

			[[nodiscard]] bool chordHeld() const
			{
				return !chord_.empty() && std::ranges::all_of( chord_, [this]( const config::KeyGroup& group ) {
					return std::ranges::any_of( group, [this]( config::Key key ) { return down_.test( static_cast<std::size_t>( key ) ); } );
				} );
			}

			void updateTrigger()
			{
				const bool held = chordHeld();
				if ( held && !trigger_active_ )
				{
					trigger_active_ = true;
					wheel_          = 0;
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

			// Recording ends when nothing that was recorded is still held.
			void finishRecordingIfReleased()
			{
				for ( int key = 0; key < key_count; ++key )
				{
					if ( down_.test( static_cast<std::size_t>( key ) ) && std::ranges::contains( recorded_, config::keyName( static_cast<config::Key>( key ) ) ) )
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
				// A pointer flying across the screen is not reading anything: it is looked at again once it slows down,
				// instead of reading every spot it passes. The previous speed counts too, so turning round is not stopping.
				const auto   now     = Clock::now();
				const double elapsed = std::chrono::duration<double>( now - probe_time_ ).count();
				const double speed   = elapsed > 0.0 && elapsed < 0.25 ? std::hypot( at.x - probe_point_.x, at.y - probe_point_.y ) / elapsed : 0.0;
				const bool   flying  = !force && std::max( speed, probe_speed_ ) > fast_pointer * scaleFactor();
				probe_point_         = at;
				probe_time_          = now;
				probe_speed_         = speed;
				if ( flying )
				{
					scan_pending_  = true;
					scan_deadline_ = now + settle_time;
					return;
				}
				last_point_ = at;
				if ( events_.scan )
				{
					events_.scan( last_point_, windowAt( last_point_ ) );
				}
			}

			WindowInfo windowInfo( HWND window )
			{
				if ( window == nullptr )
				{
					return {};
				}
				if ( const auto it = window_cache_.find( window ); it != window_cache_.end() && Clock::now() - it->second.second < std::chrono::seconds( 5 ) )
				{
					return it->second.first;
				}
				if ( window_cache_.size() > 128 )
				{
					window_cache_.clear();
				}

				WindowInfo info;
				info.id   = idOf( window );
				DWORD pid = 0;
				GetWindowThreadProcessId( window, &pid );
				info.pid = pid;
				// Like X11's "name.Class": the program and the window class, e.g. "mspaint.MSPaintApp".
				info.wm_class = programOf( pid ) + "." + classOf( window );

				RECT        rect{};
				MONITORINFO monitor{ .cbSize = sizeof( MONITORINFO ), .rcMonitor = {}, .rcWork = {}, .dwFlags = 0 };
				if ( GetWindowRect( window, &rect ) != FALSE && GetMonitorInfoW( MonitorFromWindow( window, MONITOR_DEFAULTTONEAREST ), &monitor ) != FALSE )
				{
					const RECT& m   = monitor.rcMonitor;
					info.fullscreen = rect.left <= m.left && rect.top <= m.top && rect.right >= m.right && rect.bottom >= m.bottom && ( GetWindowLongW( window, GWL_STYLE ) & WS_CAPTION ) != WS_CAPTION;
				}
				window_cache_[window] = { info, Clock::now() };
				return info;
			}

			// The popup follows the window its text came from: closing it, minimising it or moving it to another virtual
			// desktop closes the popup too.
			void watchSource( HWND window )
			{
				source_         = window;
				source_checked_ = Clock::now();
			}

			void checkSource()
			{
				source_checked_ = Clock::now();
				BOOL cloaked    = FALSE;
				// Windows on other virtual desktops are cloaked.
				( void )DwmGetWindowAttribute( source_, DWMWA_CLOAKED, &cloaked, sizeof( cloaked ) );
				if ( IsWindow( source_ ) == FALSE || IsIconic( source_ ) != FALSE || IsWindowVisible( source_ ) == FALSE || cloaked != FALSE )
				{
					source_ = nullptr;
					if ( events_.source_closed )
					{
						events_.source_closed();
					}
				}
			}

			// An area one of our windows has just left keeps counting as covered for OCR until the application below will
			// have painted it again.
			void vacate( const Rect& rect )
			{
				const auto now = Clock::now();
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
				if ( popup_shown_ )
				{
					overlays.push_back( { .rect = popup_rect_, .underneath = nullptr, .stipple = 0 } );
				}
				if ( highlight_shown_ && !transparent_.load( std::memory_order_relaxed ) )
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

			// With the compositor setting off, the highlight looks as on X11 without one: opaque lines, and for a fill a
			// highlighter mark around the glyphs (or fine dots on a textured background) that leaves the glyphs themselves
			// as the live screen pixels. The pixels it covers, as an X bitmap.
			std::vector<std::uint8_t> plainMask()
			{
				const int  w      = highlight_rect_.width;
				const int  h      = highlight_rect_.height;
				const int  stride = ( w + 7 ) / 8;
				const auto shaped = render::highlightMask( w, h, look_ );
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

			// The screen pixels under the highlight (for its automatic colour and the plain highlighter mark). Where the
			// previous highlight still covers the screen, its own capture stands in for it.
			void captureUnderlay( const Rect& rect )
			{
				const int                  w = rect.width;
				const int                  h = rect.height;
				std::vector<std::uint32_t> pixels( static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ), 0xFF000000U );
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

			void wake() const
			{
				SetEvent( wake_event_ );
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

			HWND createOverlay( DWORD extra_style )
			{
				// Layered (per-pixel alpha), topmost, never activated, and not on the taskbar or in Alt+Tab.
				HWND window = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | extra_style, window_class, L"Lexiglance", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW( nullptr ), this );
				if ( window != nullptr )
				{
					// Windows 11 may round and shadow windows itself; ours draw their own corners (Windows 10 ignores this).
					const DWORD square = do_not_round;
					( void )DwmSetWindowAttribute( window, window_corner_preference, &square, sizeof( square ) );
				}
				return window;
			}

			void ensurePopupWindow()
			{
				if ( popup_window_ == nullptr )
				{
					popup_window_ = createOverlay( 0 );
				}
			}

			void ensureHighlightWindow()
			{
				if ( highlight_window_ == nullptr )
				{
					// Click-through: every click reaches the application underneath.
					highlight_window_ = createOverlay( WS_EX_TRANSPARENT );
				}
			}

			static void raise( HWND window )
			{
				SetWindowPos( window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW );
			}

			// Clicking an entry copies its headword.
			void copy( int content_y )
			{
				const auto* region = popup_.image ? popup_.image->regionAt( content_y ) : nullptr;
				if ( region == nullptr || region->text.empty() )
				{
					return;
				}
				if ( writeClipboard( region->text ) )
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
					if ( std::string text = popup_.image->selectedText( *select_anchor_, *select_focus_ ); !text.empty() && writeClipboard( text ) )
					{
						showBadge( "Copied ✓", std::chrono::milliseconds( 900 ) );
						return;
					}
				}
				select_anchor_.reset();
				select_focus_.reset();
				paintPopup();
			}

			// The clipboard is busy for a moment now and then while another program writes it.
			bool openClipboard() const
			{
				for ( int attempt = 0; attempt < 5; ++attempt )
				{
					if ( OpenClipboard( host_ ) != FALSE )
					{
						return true;
					}
					Sleep( 10 );
				}
				return false;
			}

			bool writeClipboard( const std::string& text )
			{
				const std::wstring wide   = win32::wide( text );
				HGLOBAL            memory = GlobalAlloc( GMEM_MOVEABLE, ( wide.size() + 1 ) * sizeof( wchar_t ) );
				if ( memory == nullptr )
				{
					return false;
				}
				if ( auto* target = static_cast<wchar_t*>( GlobalLock( memory ) ); target != nullptr )
				{
					std::memcpy( target, wide.c_str(), ( wide.size() + 1 ) * sizeof( wchar_t ) );
					GlobalUnlock( memory );
				}
				if ( !openClipboard() )
				{
					GlobalFree( memory );
					return false;
				}
				EmptyClipboard();
				const bool stored = SetClipboardData( CF_UNICODETEXT, memory ) != nullptr;
				if ( !stored )
				{
					GlobalFree( memory );
				}
				CloseClipboard();
				// Our own copy is not a selection to look up.
				own_clipboard_ = GetClipboardSequenceNumber();
				return stored;
			}

			void clipboardChanged()
			{
				if ( GetClipboardSequenceNumber() == own_clipboard_ || !events_.selection || selection_mode_ == config::SelectionMode::Off )
				{
					return;
				}
				if ( selection_mode_ == config::SelectionMode::WithTrigger && !trigger_active_ )
				{
					return;
				}
				if ( IsClipboardFormatAvailable( CF_UNICODETEXT ) == FALSE || !openClipboard() )
				{
					return;
				}
				std::string text;
				if ( HANDLE data = GetClipboardData( CF_UNICODETEXT ); data != nullptr )
				{
					if ( const auto* chars = static_cast<const wchar_t*>( GlobalLock( data ) ); chars != nullptr )
					{
						text = win32::narrow( chars );
						GlobalUnlock( data );
					}
				}
				CloseClipboard();
				if ( !text.empty() )
				{
					events_.selection( std::move( text ), pointer() );
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
				popup_canvas_.present( popup_window_, popup_rect_.x, popup_rect_.y );
			}

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
					render::drawHighlight( cr, highlight_rect_.width, highlight_rect_.height, look_, c );
					cairo_destroy( cr );
				}
				else
				{
					cairo_surface_flush( surface );
					const auto          bits   = plainMask();
					const int           w      = highlight_rect_.width;
					const int           stride = ( w + 7 ) / 8;
					const std::uint32_t colour = premultiplied( look_.shape == render::HighlightShape::Fill && marker_ ? marker_color_ : c, 1.0 );
					std::uint32_t*      pixels = highlight_canvas_.pixels();
					for ( int y = 0; y < highlight_rect_.height; ++y )
					{
						for ( int x = 0; x < w; ++x )
						{
							const auto byte                                                                                           = ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( stride ) ) + static_cast<std::size_t>( x / 8 );
							const bool on                                                                                             = ( bits[byte] & ( 1U << static_cast<unsigned>( x % 8 ) ) ) != 0;
							pixels[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( w ) ) + static_cast<std::size_t>( x )] = on ? colour : 0U;
						}
					}
					cairo_surface_mark_dirty( surface );
				}
				highlight_canvas_.present( highlight_window_, highlight_rect_.x, highlight_rect_.y );
			}

			// The work area of the monitor at a point (the taskbar left out).
			static Rect monitorAt( Point point )
			{
				MONITORINFO info{ .cbSize = sizeof( MONITORINFO ), .rcMonitor = {}, .rcWork = {}, .dwFlags = 0 };
				if ( GetMonitorInfoW( MonitorFromPoint( POINT{ .x = point.x, .y = point.y }, MONITOR_DEFAULTTONEAREST ), &info ) == FALSE )
				{
					return { .x = 0, .y = 0, .width = GetSystemMetrics( SM_CXSCREEN ), .height = GetSystemMetrics( SM_CYSCREEN ) };
				}
				const RECT& r = info.rcWork;
				return { .x = r.left, .y = r.top, .width = r.right - r.left, .height = r.bottom - r.top };
			}

			// The primary monitor's scale (Settings -> Display -> Scale) and the apps' colour mode.
			void loadDesktopSettings()
			{
				UINT dpi_x = 96;
				UINT dpi_y = 96;
				if ( FAILED( GetDpiForMonitor( MonitorFromPoint( POINT{ .x = 0, .y = 0 }, MONITOR_DEFAULTTOPRIMARY ), MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y ) ) || dpi_x < 48 || dpi_x > 480 )
				{
					dpi_x = 96;
				}
				// Quarter steps keep text crisp.
				scale_.store( std::max( 0.5, std::round( dpi_x / 96.0 * 4.0 ) / 4.0 ), std::memory_order_relaxed );
				dark_.store( appsUseDarkTheme(), std::memory_order_relaxed );
			}

			HWND                          host_             = nullptr;
			HWND                          popup_window_     = nullptr;
			HWND                          highlight_window_ = nullptr;
			HANDLE                        wake_event_       = nullptr;
			std::unique_ptr<ScreenReader> screen_;
			LayeredCanvas                 popup_canvas_;
			LayeredCanvas                 highlight_canvas_;

			bool              popup_shown_ = false;
			HWND              source_      = nullptr;
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

			DWORD             own_clipboard_       = 0;
			bool              clipboard_listening_ = false;
			Clock::time_point badge_until_;
			std::string       badge_;

			config::KeyChord      chord_;
			std::bitset<32>       down_;
			bool                  trigger_active_ = false;
			int                   wheel_          = 0;
			config::SelectionMode selection_mode_ = config::SelectionMode::Off;

			std::chrono::milliseconds delay_{ 20 };
			int                       move_threshold_ = 3;
			bool                      scan_pending_   = false;
			Clock::time_point         scan_deadline_;
			Clock::time_point         last_scan_;
			Clock::time_point         probe_time_;
			Point                     probe_point_;
			double                    probe_speed_ = 0.0;
			Point                     last_point_{ .x = INT_MIN / 2, .y = INT_MIN / 2 };

			std::unordered_map<HWND, std::pair<WindowInfo, Clock::time_point>> window_cache_;

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
			std::vector<std::move_only_function<void()>>                        tasks_;
			std::atomic<bool>                                                   quit_{ false };
			std::atomic<bool>                                                   transparent_{ true };
			std::atomic<double>                                                 scale_{ 1.0 };
			std::atomic<bool>                                                   dark_{ false };
			std::atomic<Clock::rep>                                             last_input_{ 0 };
		};

	} // namespace

	Result<std::unique_ptr<Backend>> createWindowsBackend()
	{
		auto backend = std::make_unique<WindowsBackend>();
		if ( auto opened = backend->open(); !opened )
		{
			return std::unexpected( opened.error() );
		}
		return backend;
	}

} // namespace lexiglance::platform
