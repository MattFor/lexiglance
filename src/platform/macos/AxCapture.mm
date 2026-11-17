#include "AxCapture.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Utf8.h>

#include <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <numeric>
#include <ranges>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <unistd.h>

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// How long one message to an application may take: a hung one must never stall scanning for long.
		constexpr float message_timeout = 0.4F;
		// How long one capture (or one highlight) may keep asking.
		constexpr auto capture_budget = std::chrono::milliseconds( 1200 );
		// How far up from the element under the pointer the text is looked for, and how far down inside it.
		constexpr int text_depth        = 4;
		constexpr int text_search_depth = 8;
		// UTF-16 units read on each side of the pointer for its paragraph, the sentence of Anki notes.
		constexpr CFIndex paragraph_reach = 1000;

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

		// Owns one CoreFoundation reference.
		template <typename T = CFTypeRef>
		using Cf = std::unique_ptr<std::remove_pointer_t<T>, Release>;

		Cf<> attribute( AXUIElementRef element, CFStringRef name )
		{
			CFTypeRef value = nullptr;
			return AXUIElementCopyAttributeValue( element, name, &value ) == kAXErrorSuccess ? Cf<>( value ) : Cf<>();
		}

		Cf<> parameterized( AXUIElementRef element, CFStringRef name, CFTypeRef parameter )
		{
			CFTypeRef value = nullptr;
			return parameter != nullptr && AXUIElementCopyParameterizedAttributeValue( element, name, parameter, &value ) == kAXErrorSuccess ? Cf<>( value ) : Cf<>();
		}

		Cf<> rangeValue( CFIndex location, CFIndex length )
		{
			const CFRange range = CFRangeMake( location, length );
			return Cf<>( AXValueCreate( kAXValueTypeCFRange, &range ) );
		}

		Cf<> pointValue( Point point )
		{
			const CGPoint at = CGPointMake( point.x, point.y );
			return Cf<>( AXValueCreate( kAXValueTypeCGPoint, &at ) );
		}

		Cf<> number( CFIndex value )
		{
			return Cf<>( CFNumberCreate( nullptr, kCFNumberCFIndexType, &value ) );
		}

		template <typename T>
		std::optional<T> valueOf( const Cf<>& value, AXValueType type )
		{
			T out{};
			if ( value && CFGetTypeID( value.get() ) == AXValueGetTypeID() && AXValueGetValue( static_cast<AXValueRef>( value.get() ), type, &out ) )
			{
				return out;
			}
			return std::nullopt;
		}

		std::optional<CFIndex> numberOf( const Cf<>& value )
		{
			CFIndex out = 0;
			if ( value && CFGetTypeID( value.get() ) == CFNumberGetTypeID() && CFNumberGetValue( static_cast<CFNumberRef>( value.get() ), kCFNumberCFIndexType, &out ) )
			{
				return out;
			}
			return std::nullopt;
		}

		// UTF-8 of `units` UTF-16 units from the start of a string (all of it when negative); a lone surrogate becomes '?'.
		std::string utf8( const Cf<>& value, CFIndex units = -1 )
		{
			if ( !value || CFGetTypeID( value.get() ) != CFStringGetTypeID() )
			{
				return {};
			}
			const auto    string = static_cast<CFStringRef>( value.get() );
			const CFIndex length = CFStringGetLength( string );
			const CFRange range  = CFRangeMake( 0, units < 0 ? length : std::min( units, length ) );
			CFIndex       bytes  = 0;
			CFStringGetBytes( string, range, kCFStringEncodingUTF8, '?', false, nullptr, 0, &bytes );
			std::string out( static_cast<std::size_t>( bytes ), '\0' );
			CFStringGetBytes( string, range, kCFStringEncodingUTF8, '?', false, reinterpret_cast<UInt8*>( out.data() ), bytes, nullptr );
			return out;
		}

		// UTF-16 units of UTF-8 text, as the accessibility API counts characters.
		CFIndex units( std::string_view text )
		{
			CFIndex count = 0;
			for ( const char32_t c : utf8::codepoints( text ) )
			{
				count += c > 0xFFFF ? 2 : 1;
			}
			return count;
		}

		std::optional<Rect> rectOf( const Cf<>& value )
		{
			const auto box = valueOf<CGRect>( value, kAXValueTypeCGRect );
			if ( !box )
			{
				return std::nullopt;
			}
			const int  left   = static_cast<int>( std::floor( box->origin.x ) );
			const int  top    = static_cast<int>( std::floor( box->origin.y ) );
			const int  right  = static_cast<int>( std::ceil( box->origin.x + box->size.width ) );
			const int  bottom = static_cast<int>( std::ceil( box->origin.y + box->size.height ) );
			const Rect rect{ .x = left, .y = top, .width = right - left, .height = bottom - top };
			return rect.empty() ? std::nullopt : std::optional<Rect>( rect );
		}

		// Applications report the nearest character even past the end of a line or in an empty area; the pointer must be
		// on it.
		bool onCharacter( const Rect& character, Point point )
		{
			const Rect slack{ .x = character.x - 2, .y = character.y - 2, .width = character.width + 4, .height = character.height + 4 };
			return slack.contains( point );
		}

		// The paragraph around byte `offset` of `text`, and the offset within it.
		std::pair<std::string, std::size_t> paragraphAround( std::string_view text, std::size_t offset )
		{
			static constexpr std::array<std::string_view, 4> breaks{ "\n", "\r", " ", " " };
			std::size_t                                      begin = 0;
			std::size_t                                      stop  = text.size();
			for ( const std::string_view end : breaks )
			{
				if ( const auto at = offset > 0 ? text.rfind( end, offset - 1 ) : std::string_view::npos; at != std::string_view::npos && at + end.size() <= offset )
				{
					begin = std::max( begin, at + end.size() );
				}
				stop = std::min( stop, text.find( end, offset ) );
			}
			return { std::string( text.substr( begin, stop - begin ) ), offset - begin };
		}

		// The process with the frontmost ordinary window (the system-wide focused application is not always answered).
		pid_t frontmost()
		{
			const Cf<CFArrayRef> windows( CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID ) );
			for ( CFIndex i = 0; windows && i < CFArrayGetCount( windows.get() ); ++i )
			{
				const auto info  = static_cast<CFDictionaryRef>( CFArrayGetValueAtIndex( windows.get(), i ) );
				const auto layer = static_cast<CFNumberRef>( CFDictionaryGetValue( info, kCGWindowLayer ) );
				const auto owner = static_cast<CFNumberRef>( CFDictionaryGetValue( info, kCGWindowOwnerPID ) );
				int        level = -1;
				pid_t      pid   = 0;
				if ( layer != nullptr && owner != nullptr && CFNumberGetValue( layer, kCFNumberIntType, &level ) && level == 0 && CFNumberGetValue( owner, kCFNumberIntType, &pid ) && pid != getpid() )
				{
					return pid;
				}
			}
			return 0;
		}

		// How the text was found: through character ranges (AppKit, Qt, Chromium's text fields), text markers (WebKit,
		// Chromium), or as a label's value spread evenly over it.
		enum class Kind : std::uint8_t
		{
			Range,
			Markers,
			Estimate
		};

		// What bounds() needs of a capture.
		struct Source
		{
			Kind               kind = Kind::Range;
			Cf<AXUIElementRef> element;
			// Range: the UTF-16 index of the character under the pointer.
			CFIndex location = 0;
			// Markers: the positions from the start of the character under the pointer on, one per character (ahead[0] is
			// its start), and before it (behind[0] one character back), found as far as asked.
			std::vector<Cf<>> ahead;
			std::vector<Cf<>> behind;
			// Markers, where the application has character indexes: the index of the character under the pointer, and the
			// positions at indexes asked for.
			std::optional<CFIndex>  base;
			std::map<CFIndex, Cf<>> indexed;
			// Estimate: the label's frame, its characters and which of them is under the pointer.
			CGRect      frame{};
			std::size_t count = 0;
			std::size_t index = 0;
		};

		// Asks applications for their accessibility tree, and remembers which were asked; shared with work that outlives
		// the capture.
		struct Exposer
		{
			// Electron builds its pages' accessibility tree only for assistive applications, which ask like this. Chromium
			// ignores that and builds it for screen readers, which set AXEnhancedUserInterface: only Chromium gets that, as
			// other applications animate their windows while it is on. True once that is settled; an application without a
			// window yet is asked again later.
			bool expose( pid_t pid )
			{
				if ( pid <= 0 || pid == getpid() || exposed( pid, false ) )
				{
					return true;
				}
				// Asked without the lock: an application that does not answer holds up only the thread asking.
				const Cf<AXUIElementRef> application( AXUIElementCreateApplication( pid ) );
				if ( AXUIElementSetAttributeValue( application.get(), CFSTR( "AXManualAccessibility" ), kCFBooleanTrue ) == kAXErrorSuccess )
				{
					if ( !exposed( pid, true ) )
					{
						log::debug( "ax: process {} exposes its text now", pid );
					}
					return true;
				}
				// Chromium's views carry its node ids.
				const Cf<> window = attribute( application.get(), kAXFocusedWindowAttribute );
				const Cf<> views  = window && CFGetTypeID( window.get() ) == AXUIElementGetTypeID() ? attribute( static_cast<AXUIElementRef>( window.get() ), kAXChildrenAttribute ) : Cf<>();
				if ( !views || CFGetTypeID( views.get() ) != CFArrayGetTypeID() )
				{
					return false;
				}
				// Asked meanwhile from the other thread.
				if ( exposed( pid, true ) )
				{
					return true;
				}
				const auto list = static_cast<CFArrayRef>( views.get() );
				for ( CFIndex i = 0; i < CFArrayGetCount( list ); ++i )
				{
					if ( attribute( static_cast<AXUIElementRef>( CFArrayGetValueAtIndex( list, i ) ), CFSTR( "ChromeAXNodeId" ) ) )
					{
						( void )AXUIElementSetAttributeValue( application.get(), CFSTR( "AXEnhancedUserInterface" ), kCFBooleanTrue );
						log::debug( "ax: asked Chromium process {} for its pages' text", pid );
						break;
					}
				}
				return true;
			}

			// Whether a process was asked already; `settle` records that it was.
			bool exposed( pid_t pid, bool settle )
			{
				const std::scoped_lock lock( mutex );
				return settle ? !asked.insert( pid ).second : asked.contains( pid );
			}

			std::mutex                mutex;
			std::unordered_set<pid_t> asked;
			std::atomic<bool>         stopping = false;
		};

		class AxCapture final : public TextCapture
		{
		public:
			AxCapture() :
				system_( AXUIElementCreateSystemWide() )
			{
				// On the system-wide element it sets the timeout of every element.
				AXUIElementSetMessagingTimeout( system_.get(), message_timeout );
				static std::once_flag asked;
				std::call_once( asked, [] {
					// macOS's own prompt, which leads to the switch in System Settings; once per run.
					const void*               keys[]   = { kAXTrustedCheckOptionPrompt };
					const void*               values[] = { kCFBooleanTrue };
					const Cf<CFDictionaryRef> options( CFDictionaryCreate( nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks ) );
					if ( AXIsProcessTrustedWithOptions( options.get() ) == 0 )
					{
						log::warn( "accessibility: macOS has not given Lexiglance the Accessibility permission" );
					}
				} );
				// Asked as soon as an application comes to the front: Chromium and Electron take a second or two to build their
				// pages' tree, which the first lookup in them would miss otherwise.
				const std::shared_ptr<Exposer> exposer   = exposer_;
				const auto                     activated = ^( NSNotification* note ) {
				  const pid_t pid = static_cast<NSRunningApplication*>( note.userInfo[NSWorkspaceApplicationKey] ).processIdentifier;
				  // Not here: the main thread posting this waits for it.
				  dispatch_async( dispatch_get_global_queue( QOS_CLASS_UTILITY, 0 ), ^{
					// A starting application has no window for a moment.
					for ( int attempt = 0; attempt < 20 && !exposer->stopping && !exposer->expose( pid ); ++attempt )
					{
						std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
					}
				  } );
				};
				observer_ = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:nil usingBlock:activated];
			}

			~AxCapture() override
			{
				exposer_->stopping = true;
				[NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer_];
			}

			AxCapture( const AxCapture& )            = delete;
			AxCapture& operator=( const AxCapture& ) = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "accessibility";
			}

			std::optional<CapturedText> capture( Point point, const WindowInfo& window, CaptureScope scope ) override
			{
				if ( window.own || scope.characters == 0 )
				{
					return std::nullopt;
				}
				deadline_ = Clock::now() + capture_budget;
				if ( window.pid != 0 )
				{
					exposer_->expose( static_cast<pid_t>( window.pid ) );
				}
				AXUIElementRef     hit   = nullptr;
				const AXError      error = AXUIElementCopyElementAtPosition( system_.get(), static_cast<float>( point.x ), static_cast<float>( point.y ), &hit );
				Cf<AXUIElementRef> element( hit );
				if ( error != kAXErrorSuccess || !element )
				{
					log::debug( "ax: no element at {},{} (error {})", point.x, point.y, static_cast<int>( error ) );
					return std::nullopt;
				}
				pid_t pid = 0;
				AXUIElementGetPid( element.get(), &pid );
				if ( pid == getpid() )
				{
					return std::nullopt;
				}
				exposer_->expose( pid );

				for ( int depth = 0; depth < text_depth && element && !expired(); ++depth )
				{
					CFArrayRef names = nullptr;
					( void )AXUIElementCopyParameterizedAttributeNames( element.get(), &names );
					const Cf<CFArrayRef> owned( names );
					const auto           offers  = [&]( CFStringRef name ) { return names != nullptr && CFArrayContainsValue( names, CFRangeMake( 0, CFArrayGetCount( names ) ), name ) != 0; };
					const bool           markers = offers( CFSTR( "AXTextMarkerForPosition" ) );
					if ( offers( kAXRangeForPositionParameterizedAttribute ) )
					{
						// Chromium's text fields list it without answering it; their text markers work.
						auto captured = byRange( element.get(), point, scope );
						if ( captured || !markers )
						{
							return captured;
						}
					}
					if ( markers )
					{
						return byMarkers( std::move( element ), point, scope );
					}
					if ( depth == 0 && utf8( attribute( element.get(), kAXRoleAttribute ) ) == "AXStaticText" )
					{
						return byEstimate( std::move( element ), point, scope );
					}
					AXUIElementRef parent = nullptr;
					if ( AXUIElementCopyAttributeValue( element.get(), kAXParentAttribute, reinterpret_cast<CFTypeRef*>( &parent ) ) != kAXErrorSuccess )
					{
						break;
					}
					element.reset( parent );
				}
				log::debug( "ax: nothing at {},{} exposes text", point.x, point.y );
				return std::nullopt;
			}

			std::optional<Rect> bounds( const CapturedText& captured, std::size_t length ) override
			{
				const auto lines = lineBounds( captured, length );
				if ( lines.empty() )
				{
					return std::nullopt;
				}
				Rect all = lines.front();
				for ( const Rect& line : lines )
				{
					all = all.united( line );
				}
				return all;
			}

			std::vector<Rect> lineBounds( const CapturedText& captured, std::size_t length ) override
			{
				auto* source = static_cast<Source*>( captured.handle.get() );
				if ( source == nullptr || length == 0 )
				{
					return {};
				}
				deadline_ = Clock::now() + capture_budget;
				switch ( source->kind )
				{
					case Kind::Range:
						return rangeLines( *source, captured, length );
					case Kind::Markers:
						return markerLines( *source, captured, length );
					case Kind::Estimate:
						break;
				}
				const double      advance = source->frame.size.width / static_cast<double>( source->count );
				const std::size_t first   = source->index - std::min( source->index, captured.rewound );
				const std::size_t shown   = std::min( length, source->count - first );
				const int         left    = static_cast<int>( std::floor( source->frame.origin.x + ( static_cast<double>( first ) * advance ) ) );
				const int         right   = static_cast<int>( std::ceil( source->frame.origin.x + ( static_cast<double>( first + shown ) * advance ) ) );
				return { Rect{ .x = left, .y = static_cast<int>( source->frame.origin.y ), .width = right - left, .height = static_cast<int>( std::ceil( source->frame.size.height ) ) } };
			}

			void diagnose( std::vector<health::Check>& out ) override
			{
				health::Check check{ .id = "accessibility", .title = "Text in applications (accessibility)" };
				if ( AXIsProcessTrusted() == 0 )
				{
					check.status = health::Severity::Error;
					check.detail = "Lexiglance doesn't have the Accessibility permission, so it can't read text in applications (only OCR and selections "
								   "work). Turn Lexiglance on in System Settings -> Privacy & Security -> Accessibility.";
					check.fix    = "open-privacy-accessibility";
					out.push_back( std::move( check ) );
					return;
				}
				// A round trip to the application in front.
				const Cf<AXUIElementRef> front( AXUIElementCreateApplication( frontmost() ) );
				const auto               started  = Clock::now();
				const bool               answered = attribute( front.get(), kAXRoleAttribute ) != nullptr;
				const auto               elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - started ).count();
				check.status                      = !answered || elapsed > 250 ? health::Severity::Warning : health::Severity::Ok;
				check.detail                      = answered ? std::format( "The application in front answered in {} ms. Text is read from applications that expose it (AppKit, Safari and other WebKit views, Chromium and Electron, Qt); OCR reads the rest.", elapsed )
				                                             : std::string( "The application in front didn't answer through accessibility, so OCR reads its text." );
				if ( answered && elapsed > 250 )
				{
					check.detail += " That's slow, so lookups in applications will lag.";
				}
				out.push_back( std::move( check ) );
			}

		private:
			[[nodiscard]] bool expired() const
			{
				return Clock::now() >= deadline_;
			}

			// Character ranges: AppKit text views (TextEdit, Notes, Mail, Terminal), Qt, text fields.
			std::optional<CapturedText> byRange( AXUIElementRef e, Point point, CaptureScope scope )
			{
				const auto range = valueOf<CFRange>( parameterized( e, kAXRangeForPositionParameterizedAttribute, pointValue( point ).get() ), kAXValueTypeCFRange );
				if ( !range || range->location < 0 )
				{
					return std::nullopt;
				}
				const auto character = rectOf( parameterized( e, kAXBoundsForRangeParameterizedAttribute, rangeValue( range->location, std::max<CFIndex>( range->length, 1 ) ).get() ) );
				if ( !character || !onCharacter( *character, point ) )
				{
					log::debug( "ax: {},{} is beside the nearest character", point.x, point.y );
					return std::nullopt;
				}
				// The paragraph around it, at most paragraph_reach units to each side.
				const auto    total            = numberOf( attribute( e, kAXNumberOfCharactersAttribute ) );
				const CFIndex begin            = std::max<CFIndex>( 0, range->location - paragraph_reach );
				const CFIndex end              = total ? std::min( *total, range->location + paragraph_reach ) : range->location + std::max<CFIndex>( range->length, 1 );
				const auto    text             = parameterized( e, kAXStringForRangeParameterizedAttribute, rangeValue( begin, end - begin ).get() );
				const auto [paragraph, offset] = paragraphAround( utf8( text ), utf8( text, range->location - begin ).size() );
				CapturedText captured{ .text = std::string( utf8::prefix( std::string_view( paragraph ).substr( offset ), scope.characters ) ), .offset = static_cast<std::int32_t>( range->location ), .character = *character };
				if ( captured.text.empty() )
				{
					return std::nullopt;
				}
				captured.sentence        = paragraph;
				captured.sentence_offset = offset;
				captured.handle          = std::make_shared<Source>( Source{ .kind = Kind::Range, .element = Cf<AXUIElementRef>( static_cast<AXUIElementRef>( CFRetain( e ) ) ), .location = range->location } );
				return captured;
			}

			// Text markers: web content in WebKit (Safari, Mail's messages) and Chromium (Chrome, Electron).
			std::optional<CapturedText> byMarkers( Cf<AXUIElementRef> element, Point point, CaptureScope scope )
			{
				AXUIElementRef e      = element.get();
				Cf<>           marker = parameterized( e, CFSTR( "AXTextMarkerForPosition" ), pointValue( point ).get() );
				if ( !marker )
				{
					if ( auto text = textUnder( e, point, 0 ) )
					{
						element = std::move( text );
						e       = element.get();
					}
					marker = markerInText( e, point );
				}
				if ( !marker )
				{
					log::debug( "ax: no text position at {},{}", point.x, point.y );
					return std::nullopt;
				}
				// The caret position nearest the pointer: the character under it starts there, or ends there when the pointer
				// is on its right half.
				Cf<>                start;
				Cf<>                next      = parameterized( e, CFSTR( "AXNextTextMarkerForTextMarker" ), marker.get() );
				std::optional<Rect> character = markerBounds( e, marker.get(), next.get() );
				if ( character && onCharacter( *character, point ) )
				{
					start = std::move( marker );
				}
				else
				{
					start     = parameterized( e, CFSTR( "AXPreviousTextMarkerForTextMarker" ), marker.get() );
					character = markerBounds( e, start.get(), marker.get() );
					next      = std::move( marker );
					if ( !character || !onCharacter( *character, point ) )
					{
						log::debug( "ax: {},{} is beside the nearest character", point.x, point.y );
						return std::nullopt;
					}
				}
				// From there to the end of its paragraph, which the whole paragraph ends with: that tells where it starts.
				const Cf<>  end       = parameterized( e, CFSTR( "AXNextParagraphEndTextMarkerForTextMarker" ), start.get() );
				std::string rest      = utf8( parameterized( e, CFSTR( "AXStringForTextMarkerRange" ), markerRange( e, start.get(), end.get() ).get() ) );
				std::string paragraph = utf8( parameterized( e, CFSTR( "AXStringForTextMarkerRange" ), parameterized( e, CFSTR( "AXParagraphTextMarkerRangeForTextMarker" ), start.get() ).get() ) );
				for ( std::string* text : { &rest, &paragraph } )
				{
					while ( !text->empty() && ( text->back() == '\n' || text->back() == '\r' ) )
					{
						text->pop_back();
					}
				}
				if ( rest.empty() )
				{
					rest = utf8( parameterized( e, CFSTR( "AXStringForTextMarkerRange" ), markerRange( e, start.get(), next.get() ).get() ) );
				}
				std::size_t offset = 0;
				if ( !rest.empty() && paragraph.ends_with( rest ) )
				{
					offset = paragraph.size() - rest.size();
				}
				else
				{
					paragraph = rest;
				}
				const auto [sentence, at] = paragraphAround( paragraph, offset );
				CapturedText captured{ .text = std::string( utf8::prefix( std::string_view( sentence ).substr( at ), scope.characters ) ), .offset = 0, .character = *character };
				if ( captured.text.empty() )
				{
					return std::nullopt;
				}
				captured.sentence        = sentence;
				captured.sentence_offset = at;
				auto source              = std::make_shared<Source>( Source{ .kind = Kind::Markers, .element = std::move( element ), .base = numberOf( parameterized( e, CFSTR( "AXIndexForTextMarker" ), start.get() ) ) } );
				// Chromium counts AXIndexForTextMarker in the node and AXTextMarkerForIndex in the page (where a position it
				// cannot place has the page's bounds), WebKit both in the page: the index must lead back to the character.
				if ( source->base )
				{
					const Point middle{ .x = character->x + ( character->width / 2 ), .y = character->y + ( character->height / 2 ) };
					const auto  check = markerBounds( e, markerAtIndex( *source, *source->base ), markerAtIndex( *source, *source->base + 1 ) );
					if ( !check || !check->contains( middle ) || check->width > character->width * 2 || check->height > character->height * 2 )
					{
						source->base.reset();
						source->indexed.clear();
					}
				}
				source->ahead.push_back( std::move( start ) );
				source->ahead.push_back( std::move( next ) );
				captured.handle = std::move( source );
				return captured;
			}

			// The element with text under the point, this one or one inside it: Chromium answers a hit test on a part of the
			// page it has not hit-tested before with what contains it.
			Cf<AXUIElementRef> textUnder( AXUIElementRef e, Point point, int depth ) const
			{
				if ( const Cf<> value = attribute( e, kAXValueAttribute ); value && CFGetTypeID( value.get() ) == CFStringGetTypeID() && CFStringGetLength( static_cast<CFStringRef>( value.get() ) ) > 0 )
				{
					return Cf<AXUIElementRef>( static_cast<AXUIElementRef>( CFRetain( e ) ) );
				}
				const Cf<> children = depth < text_search_depth ? attribute( e, kAXChildrenAttribute ) : Cf<>();
				for ( CFIndex i = 0; children && CFGetTypeID( children.get() ) == CFArrayGetTypeID() && i < CFArrayGetCount( static_cast<CFArrayRef>( children.get() ) ) && !expired(); ++i )
				{
					const auto child = static_cast<AXUIElementRef>( CFArrayGetValueAtIndex( static_cast<CFArrayRef>( children.get() ), i ) );
					if ( const auto frame = rectOf( attribute( child, CFSTR( "AXFrame" ) ) ); frame && frame->contains( point ) )
					{
						if ( auto found = textUnder( child, point, depth + 1 ) )
						{
							return found;
						}
					}
				}
				return {};
			}

			// Chromium lists AXTextMarkerForPosition without answering it: the character under the pointer is found in the
			// text element by halving over its characters' bounds, then reached by stepping from the element's start.
			Cf<> markerInText( AXUIElementRef e, Point point ) const
			{
				const Cf<> value = attribute( e, kAXValueAttribute );
				if ( !value || CFGetTypeID( value.get() ) != CFStringGetTypeID() )
				{
					return {};
				}
				CFIndex low  = 0;
				CFIndex high = CFStringGetLength( static_cast<CFStringRef>( value.get() ) );
				while ( low < high && !expired() )
				{
					const CFIndex middle = std::midpoint( low, high );
					const auto    box    = rectOf( parameterized( e, kAXBoundsForRangeParameterizedAttribute, rangeValue( middle, 1 ).get() ) );
					if ( !box )
					{
						return {};
					}
					const bool line = point.y >= box->y && point.y < box->y + box->height;
					if ( point.y < box->y || ( line && point.x < box->x ) )
					{
						high = middle;
					}
					else if ( !line || point.x >= box->x + box->width )
					{
						low = middle + 1;
					}
					else
					{
						const Cf<> range = parameterized( e, CFSTR( "AXTextMarkerRangeForUIElement" ), e );
						if ( !range || CFGetTypeID( range.get() ) != AXTextMarkerRangeGetTypeID() )
						{
							return {};
						}
						Cf<> marker( AXTextMarkerRangeCopyStartMarker( static_cast<AXTextMarkerRangeRef>( range.get() ) ) );
						for ( std::size_t step = utf8::length( utf8( value, middle ) ); marker && step > 0 && !expired(); --step )
						{
							marker = parameterized( e, CFSTR( "AXNextTextMarkerForTextMarker" ), marker.get() );
						}
						return marker;
					}
				}
				return {};
			}

			// A label without character ranges: its value, spread evenly over one line.
			std::optional<CapturedText> byEstimate( Cf<AXUIElementRef> element, Point point, CaptureScope scope )
			{
				AXUIElementRef    e        = element.get();
				const std::string value    = utf8( attribute( e, kAXValueAttribute ) );
				const auto        position = valueOf<CGPoint>( attribute( e, kAXPositionAttribute ), kAXValueTypeCGPoint );
				const auto        size     = valueOf<CGSize>( attribute( e, kAXSizeAttribute ), kAXValueTypeCGSize );
				const std::size_t count    = utf8::length( value );
				if ( count == 0 || !position || !size || value.find_first_of( "\r\n" ) != std::string::npos )
				{
					return std::nullopt;
				}
				const double advance = size->width / static_cast<double>( count );
				// Characters wider than the line is tall: a label wider than its text. Narrower than a quarter: several lines.
				// Where each character is cannot be told then.
				if ( advance > size->height || advance * 4 < size->height || point.x < position->x || point.x >= position->x + size->width )
				{
					return std::nullopt;
				}
				const auto   index = std::min( count - 1, static_cast<std::size_t>( ( point.x - position->x ) / advance ) );
				const auto   at    = utf8::offsetOf( value, index );
				const int    left  = static_cast<int>( std::floor( position->x + ( static_cast<double>( index ) * advance ) ) );
				CapturedText captured{
					.text      = std::string( utf8::prefix( std::string_view( value ).substr( at ), scope.characters ) ),
					.offset    = static_cast<std::int32_t>( index ),
					.character = { .x = left, .y = static_cast<int>( position->y ), .width = std::max( 1, static_cast<int>( std::lround( advance ) ) ), .height = static_cast<int>( std::ceil( size->height ) ) },
				};
				captured.sentence        = value;
				captured.sentence_offset = at;
				captured.handle          = std::make_shared<Source>( Source{ .kind = Kind::Estimate, .element = std::move( element ), .frame = { *position, *size }, .count = count, .index = index } );
				return captured;
			}

			std::vector<Rect> rangeLines( const Source& source, const CapturedText& captured, std::size_t length ) const
			{
				AXUIElementRef e = source.element.get();
				// In UTF-16 units: back over the beginning of the word put in front, then the characters asked for.
				const CFIndex begin = std::max<CFIndex>( 0, source.location - units( utf8::prefix( captured.text, captured.rewound ) ) );
				const CFIndex end   = begin + std::max<CFIndex>( 1, units( utf8::prefix( captured.text, length ) ) );
				const auto    span  = [&]( CFIndex from, CFIndex to ) { return rectOf( parameterized( e, kAXBoundsForRangeParameterizedAttribute, rangeValue( from, to - from ).get() ) ); };
				const auto    whole = span( begin, end );
				// No taller than the character under the pointer and a half: one line.
				if ( !whole || whole->height <= captured.character.height * 3 / 2 )
				{
					return whole ? std::vector<Rect>{ *whole } : std::vector<Rect>{};
				}
				// Several: where the characters' tops move down, found by halving. Not by AXLineForIndex, which Qt answers
				// with the paragraph.
				std::map<CFIndex, double> tops;
				const auto                top = [&]( CFIndex at ) {
					const auto [slot, added] = tops.try_emplace( at, -1.0 );
					if ( added )
					{
						if ( const auto box = valueOf<CGRect>( parameterized( e, kAXBoundsForRangeParameterizedAttribute, rangeValue( at, 1 ).get() ), kAXValueTypeCGRect ) )
						{
							slot->second = box->origin.y;
						}
					}
					return slot->second;
				};
				const double      slack = captured.character.height / 2.0;
				std::vector<Rect> out;
				for ( CFIndex from = begin; from < end && !expired(); )
				{
					const double  line  = top( from );
					const auto    later = std::views::iota( from + 1, end );
					const auto    next  = std::ranges::partition_point( later, [&]( CFIndex at ) { return top( at ) <= line + slack; } );
					const CFIndex to    = next == later.end() ? end : *next;
					if ( const auto box = span( from, to ) )
					{
						out.push_back( *box );
					}
					from = to;
				}
				return out;
			}

			std::vector<Rect> markerLines( Source& source, const CapturedText& captured, std::size_t length )
			{
				AXUIElementRef e = source.element.get();
				// The position before character `at` of the text (after the last one when `at` is `length`).
				const auto lead = units( utf8::prefix( captured.text, captured.rewound ) );
				const auto mark = [&]( std::size_t at ) {
					return source.base ? markerAtIndex( source, *source.base - lead + units( utf8::prefix( captured.text, at ) ) ) : markerAt( source, static_cast<std::ptrdiff_t>( at ) - static_cast<std::ptrdiff_t>( captured.rewound ) );
				};
				const auto whole = markerBounds( e, mark( 0 ), mark( length ) );
				if ( !whole )
				{
					return {};
				}
				// No taller than the character under the pointer and a half: one line.
				if ( whole->height <= captured.character.height * 3 / 2 )
				{
					return { *whole };
				}
				// Several: where the characters' tops move down, found by halving. Not by AXLineForTextMarker, which Chromium
				// answers with the lines of the element asked.
				std::vector<std::optional<double>> tops( length );
				const auto                         top = [&]( std::size_t at ) {
					if ( !tops[at] )
					{
						const auto box = markerBounds( e, mark( at ), mark( at + 1 ) );
						tops[at]       = box ? box->y : -1.0;
					}
					return *tops[at];
				};
				const double      slack = captured.character.height / 2.0;
				std::vector<Rect> out;
				for ( std::size_t from = 0; from < length && !expired(); )
				{
					const auto        later = std::views::iota( from + 1, length );
					const auto        next  = std::ranges::partition_point( later, [&]( std::size_t at ) { return top( at ) <= top( from ) + slack; } );
					const std::size_t to    = next == later.end() ? length : *next;
					// Up to the start of its last character, then that one if it is on the line: a space where the line wraps is
					// put at the start of the next one.
					auto box  = to - from > 1 ? markerBounds( e, mark( from ), mark( to - 1 ) ) : std::nullopt;
					auto tail = markerBounds( e, mark( to - 1 ), mark( to ) );
					if ( tail && ( !box || ( tail->y + ( tail->height / 2 ) >= box->y && tail->y + ( tail->height / 2 ) < box->y + box->height ) ) )
					{
						// WebKit makes the last character before a wrap a caret's width: as wide as the others on the line then.
						if ( box && to < length && tail->width * 3 < tail->height )
						{
							tail->width = std::max( tail->width, box->width / static_cast<int>( to - 1 - from ) );
						}
						box = box ? box->united( *tail ) : *tail;
					}
					if ( box )
					{
						out.push_back( *box );
					}
					from = to;
				}
				return out;
			}

			// The position at a character index of the application's text (WebKit counts the space where a line wraps,
			// which stepping skips).
			static CFTypeRef markerAtIndex( Source& source, CFIndex index )
			{
				index      = std::max<CFIndex>( 0, index );
				auto& slot = source.indexed[index];
				if ( !slot )
				{
					slot = parameterized( source.element.get(), CFSTR( "AXTextMarkerForIndex" ), number( index ).get() );
				}
				return slot.get();
			}

			// The position `step` characters from the start of the one under the pointer (before it when negative), or the
			// farthest one there is, for applications without character indexes. Steps are characters as the application
			// counts them, mostly code points.
			CFTypeRef markerAt( Source& source, std::ptrdiff_t step ) const
			{
				auto&             list = step >= 0 ? source.ahead : source.behind;
				const std::size_t want = step >= 0 ? static_cast<std::size_t>( step ) : static_cast<std::size_t>( -step - 1 );
				const CFStringRef move = step >= 0 ? CFSTR( "AXNextTextMarkerForTextMarker" ) : CFSTR( "AXPreviousTextMarkerForTextMarker" );
				while ( list.size() <= want && !expired() )
				{
					CFTypeRef from  = list.empty() ? source.ahead.front().get() : list.back().get();
					Cf<>      moved = parameterized( source.element.get(), move, from );
					if ( !moved || CFEqual( moved.get(), from ) != 0 )
					{
						break;
					}
					list.push_back( std::move( moved ) );
				}
				if ( list.size() > want )
				{
					return list[want].get();
				}
				return list.empty() ? source.ahead.front().get() : list.back().get();
			}

			static Cf<> markerRange( AXUIElementRef element, CFTypeRef from, CFTypeRef to )
			{
				if ( from == nullptr || to == nullptr )
				{
					return {};
				}
				const void*          both[] = { from, to };
				const Cf<CFArrayRef> pair( CFArrayCreate( nullptr, both, 2, &kCFTypeArrayCallBacks ) );
				return parameterized( element, CFSTR( "AXTextMarkerRangeForUnorderedTextMarkers" ), pair.get() );
			}

			static std::optional<Rect> markerBounds( AXUIElementRef element, CFTypeRef from, CFTypeRef to )
			{
				return rectOf( parameterized( element, CFSTR( "AXBoundsForTextMarkerRange" ), markerRange( element, from, to ).get() ) );
			}

			Cf<AXUIElementRef>       system_;
			std::shared_ptr<Exposer> exposer_ = std::make_shared<Exposer>();
			id                       observer_;
			Clock::time_point        deadline_;
		};

	} // namespace

	std::unique_ptr<TextCapture> createAxCapture()
	{
		return std::make_unique<AxCapture>();
	}

} // namespace lexiglance::platform
