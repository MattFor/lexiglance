#include "UiaCapture.h"

#include "Win32.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Utf8.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

#include <ole2.h>
// After ole2.h, which it needs.
#include <uiautomation.h>

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// CUIAutomation, defined here because not every SDK's import libraries do.
		constexpr CLSID automation_class{ 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };

		// How far up from the element under the pointer the text pattern is looked for (browsers hit-test to a text leaf
		// inside the document that has it).
		constexpr int pattern_depth = 12;
		// The surrounding paragraph is read up to this many UTF-16 units, for the sentence of Anki notes.
		constexpr int paragraph_limit = 2000;

		// Owns one COM reference.
		template <typename T>
		class Com
		{
		public:
			Com() noexcept = default;

			~Com()
			{
				reset();
			}

			Com( const Com& other ) noexcept :
				pointer_( other.pointer_ )
			{
				if ( pointer_ != nullptr )
				{
					pointer_->AddRef();
				}
			}

			Com& operator=( const Com& other ) noexcept
			{
				if ( this != &other )
				{
					reset();
					pointer_ = other.pointer_;
					if ( pointer_ != nullptr )
					{
						pointer_->AddRef();
					}
				}
				return *this;
			}

			Com( Com&& other ) noexcept :
				pointer_( std::exchange( other.pointer_, nullptr ) )
			{
			}

			Com& operator=( Com&& other ) noexcept
			{
				if ( this != &other )
				{
					reset();
					pointer_ = std::exchange( other.pointer_, nullptr );
				}
				return *this;
			}

			[[nodiscard]] T* get() const noexcept
			{
				return pointer_;
			}

			T* operator->() const noexcept
			{
				return pointer_;
			}

			explicit operator bool() const noexcept
			{
				return pointer_ != nullptr;
			}

			// For out-parameters.
			[[nodiscard]] T** put() noexcept
			{
				reset();
				return &pointer_;
			}

			[[nodiscard]] void** putVoid() noexcept
			{
				return reinterpret_cast<void**>( put() );
			}

			[[nodiscard]] T* release() noexcept
			{
				return std::exchange( pointer_, nullptr );
			}

			void reset() noexcept
			{
				if ( pointer_ != nullptr )
				{
					pointer_->Release();
					pointer_ = nullptr;
				}
			}

		private:
			T* pointer_ = nullptr;
		};

		// A BSTR out-parameter, freed when done.
		class Bstr
		{
		public:
			Bstr() noexcept = default;

			~Bstr()
			{
				SysFreeString( value_ );
			}

			Bstr( const Bstr& )            = delete;
			Bstr& operator=( const Bstr& ) = delete;
			Bstr( Bstr&& )                 = delete;
			Bstr& operator=( Bstr&& )      = delete;

			[[nodiscard]] BSTR* put() noexcept
			{
				SysFreeString( value_ );
				value_ = nullptr;
				return &value_;
			}

			[[nodiscard]] std::string utf8() const
			{
				return value_ == nullptr ? std::string() : win32::narrow( std::wstring_view( value_, SysStringLen( value_ ) ) );
			}

		private:
			BSTR value_ = nullptr;
		};

		// The rectangles a range covers on screen, one per line (GetBoundingRectangles gives x, y, width, height each).
		std::vector<Rect> rectanglesOf( IUIAutomationTextRange* range )
		{
			std::vector<Rect> out;
			SAFEARRAY*        array = nullptr;
			if ( FAILED( range->GetBoundingRectangles( &array ) ) || array == nullptr )
			{
				return out;
			}
			LONG    lower  = 0;
			LONG    upper  = -1;
			double* values = nullptr;
			if ( SUCCEEDED( SafeArrayGetLBound( array, 1, &lower ) ) && SUCCEEDED( SafeArrayGetUBound( array, 1, &upper ) ) && SUCCEEDED( SafeArrayAccessData( array, reinterpret_cast<void**>( &values ) ) ) )
			{
				const auto count = static_cast<std::size_t>( std::max<LONG>( 0, upper - lower + 1 ) );
				for ( std::size_t i = 0; i + 3 < count; i += 4 )
				{
					const int  left   = static_cast<int>( std::floor( values[i] ) );
					const int  top    = static_cast<int>( std::floor( values[i + 1] ) );
					const int  right  = static_cast<int>( std::ceil( values[i] + values[i + 2] ) );
					const int  bottom = static_cast<int>( std::ceil( values[i + 1] + values[i + 3] ) );
					const Rect rect{ .x = left, .y = top, .width = right - left, .height = bottom - top };
					if ( !rect.empty() )
					{
						out.push_back( rect );
					}
				}
				SafeArrayUnaccessData( array );
			}
			SafeArrayDestroy( array );
			return out;
		}

		Rect unite( const std::vector<Rect>& rects )
		{
			Rect area = rects.front();
			for ( const Rect& r : rects )
			{
				const int right  = std::max( area.x + area.width, r.x + r.width );
				const int bottom = std::max( area.y + area.height, r.y + r.height );
				area.x           = std::min( area.x, r.x );
				area.y           = std::min( area.y, r.y );
				area.width       = right - area.x;
				area.height      = bottom - area.y;
			}
			return area;
		}

		// The sentence around byte `offset` of a paragraph (UI Automation has no sentence unit): from the end of the one
		// before to the end of this one.
		std::pair<std::string, std::size_t> sentenceAround( std::string_view paragraph, std::size_t offset )
		{
			static constexpr std::array<std::string_view, 7> ends{ "。", "！", "？", "!", "?", "\n", "\r" };
			std::size_t                                      begin = 0;
			std::size_t                                      stop  = paragraph.size();
			for ( const std::string_view end : ends )
			{
				if ( offset > 0 )
				{
					if ( const auto at = paragraph.rfind( end, offset - 1 ); at != std::string_view::npos && at + end.size() <= offset )
					{
						begin = std::max( begin, at + end.size() );
					}
				}
				if ( const auto at = paragraph.find( end, offset ); at != std::string_view::npos )
				{
					stop = std::min( stop, end == "\n" || end == "\r" ? at : at + end.size() );
				}
			}
			while ( begin < offset && ( paragraph[begin] == ' ' || paragraph[begin] == '\t' || paragraph.substr( begin ).starts_with( "　" ) ) )
			{
				begin += paragraph[begin] == ' ' || paragraph[begin] == '\t' ? 1 : std::string_view( "　" ).size();
			}
			return { std::string( paragraph.substr( begin, stop - begin ) ), offset - begin };
		}

		// An element for messages: its class and control type.
		std::string elementName( IUIAutomationElement* element )
		{
			Bstr          class_name;
			CONTROLTYPEID type = 0;
			( void )element->get_CurrentClassName( class_name.put() );
			( void )element->get_CurrentControlType( &type );
			return std::format( "\"{}\" (control type {})", class_name.utf8(), type );
		}

		class UiaCapture final : public TextCapture
		{
		public:
			UiaCapture()
			{
				const HRESULT joined = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
				com_                 = SUCCEEDED( joined );
				if ( FAILED( CoCreateInstance( automation_class, nullptr, CLSCTX_INPROC_SERVER, __uuidof( IUIAutomation ), automation_.putVoid() ) ) || !automation_ )
				{
					log::warn( "accessibility: UI Automation is unavailable" );
					return;
				}
				// A hung application must never stall scanning for long.
				Com<IUIAutomation2> timeouts;
				if ( SUCCEEDED( automation_->QueryInterface( __uuidof( IUIAutomation2 ), timeouts.putVoid() ) ) && timeouts )
				{
					( void )timeouts->put_ConnectionTimeout( 300 );
					( void )timeouts->put_TransactionTimeout( 1200 );
				}
				( void )automation_->get_RawViewWalker( walker_.put() );
			}

			~UiaCapture() override
			{
				walker_.reset();
				automation_.reset();
				if ( com_ )
				{
					CoUninitialize();
				}
			}

			UiaCapture( const UiaCapture& )            = delete;
			UiaCapture& operator=( const UiaCapture& ) = delete;
			UiaCapture( UiaCapture&& )                 = delete;
			UiaCapture& operator=( UiaCapture&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "ui-automation";
			}

			std::optional<CapturedText> capture( Point point, const WindowInfo& window, CaptureScope scope ) override
			{
				const std::size_t max_chars = scope.characters;
				if ( !automation_ || window.own || max_chars == 0 )
				{
					return std::nullopt;
				}
				const POINT               at{ .x = point.x, .y = point.y };
				Com<IUIAutomationElement> element;
				if ( FAILED( automation_->ElementFromPoint( at, element.put() ) ) || !element )
				{
					log::debug( "uia: no element at {},{}", point.x, point.y );
					return std::nullopt;
				}
				const auto pattern = textPatternOf( element );
				if ( !pattern )
				{
					log::debug( "uia: {} at {},{} exposes no text", elementName( element.get() ), point.x, point.y );
					return std::nullopt;
				}

				Com<IUIAutomationTextRange> range;
				if ( FAILED( pattern->RangeFromPoint( at, range.put() ) ) || !range || FAILED( range->ExpandToEnclosingUnit( TextUnit_Character ) ) )
				{
					log::debug( "uia: {} has no text at {},{}", elementName( element.get() ), point.x, point.y );
					return std::nullopt;
				}
				// Providers report the nearest character even past the end of a line or in an empty area; require the pointer
				// to be on it.
				const auto boxes = rectanglesOf( range.get() );
				if ( boxes.empty() )
				{
					log::debug( "uia: the character at {},{} in {} has no bounds", point.x, point.y, elementName( element.get() ) );
					return std::nullopt;
				}
				const Rect character = boxes.front();
				const Rect slack{ .x = character.x - 2, .y = character.y - 2, .width = character.width + 4, .height = character.height + 4 };
				if ( !slack.contains( point ) )
				{
					log::debug( "uia: {},{} is beside the nearest character ({},{} {}x{})", point.x, point.y, character.x, character.y, character.width, character.height );
					return std::nullopt;
				}

				Com<IUIAutomationTextRange> span;
				if ( FAILED( range->Clone( span.put() ) ) || !span )
				{
					return std::nullopt;
				}
				int moved = 0;
				// Characters are UTF-16 units to most providers: enough of them for max_chars characters beyond the BMP too.
				( void )span->MoveEndpointByUnit( TextPatternRangeEndpoint_End, TextUnit_Character, static_cast<int>( max_chars * 2 ) - 1, &moved );
				Bstr content;
				if ( FAILED( span->GetText( -1, content.put() ) ) )
				{
					return std::nullopt;
				}
				std::string text = content.utf8();
				// A line break ends what is looked up (a soft wrap has none).
				if ( const auto end = text.find_first_of( "\r\n" ); end != std::string::npos )
				{
					text.resize( end );
				}
				if ( text.empty() )
				{
					return std::nullopt;
				}

				CapturedText captured{ .text = std::string( utf8::prefix( text, max_chars ) ), .offset = 0, .character = character };
				sentenceOf( range.get(), captured );
				captured.handle = std::shared_ptr<void>( range.release(), []( void* object ) { static_cast<IUIAutomationTextRange*>( object )->Release(); } );
				return captured;
			}

			std::optional<Rect> bounds( const CapturedText& captured, std::size_t length ) override
			{
				auto* start = static_cast<IUIAutomationTextRange*>( captured.handle.get() );
				if ( start == nullptr || length == 0 )
				{
					return std::nullopt;
				}
				Com<IUIAutomationTextRange> span;
				if ( FAILED( start->Clone( span.put() ) ) || !span )
				{
					return std::nullopt;
				}
				// The characters of the first `length` code points of the captured text, which may start before the one the
				// range holds (the beginning of its word).
				const auto head  = utf8::prefix( captured.text, length );
				const auto units = static_cast<int>( std::max<std::size_t>( win32::wide( head ).size(), 1 ) );
				const auto lead  = static_cast<int>( win32::wide( utf8::prefix( captured.text, captured.rewound ) ).size() );
				int        back  = 0;
				int        moved = 0;
				if ( lead > 0 )
				{
					// As far as the provider went back (not past the start of its text).
					( void )span->MoveEndpointByUnit( TextPatternRangeEndpoint_Start, TextUnit_Character, -lead, &back );
				}
				( void )span->MoveEndpointByUnit( TextPatternRangeEndpoint_End, TextUnit_Character, units + std::min( back, 0 ) - 1, &moved );
				const auto boxes = rectanglesOf( span.get() );
				if ( boxes.empty() )
				{
					return std::nullopt;
				}
				return unite( boxes );
			}

			void diagnose( std::vector<health::Check>& out ) override
			{
				health::Check check{ .id = "accessibility", .title = "Text in applications (UI Automation)" };
				if ( !automation_ )
				{
					check.status = health::Severity::Error;
					check.detail = "UI Automation could not be started, so text in ordinary applications is not read (only OCR and selections work). Restarting "
								   "Lexiglance tries again.";
					check.fix    = "restart";
					out.push_back( std::move( check ) );
					return;
				}
				// A round trip to the application with the keyboard focus.
				const auto                started = Clock::now();
				Com<IUIAutomationElement> focused;
				const bool                answered = SUCCEEDED( automation_->GetFocusedElement( focused.put() ) ) && focused;
				Bstr                      name;
				if ( answered )
				{
					( void )focused->get_CurrentName( name.put() );
				}
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - started ).count();
				check.status       = !answered || elapsed > 250 ? health::Severity::Warning : health::Severity::Ok;
				check.detail       = answered ? std::format( "UI Automation answered in {} ms. Text is read from applications that support its text pattern (Office, browsers, Qt, WPF, WinUI, consoles); OCR reads the rest.", elapsed )
				                              : std::string( "The application with the keyboard focus did not answer UI Automation; its text is read with OCR." );
				if ( answered && elapsed > 250 )
				{
					check.detail += " That is slow: lookups in applications will lag.";
				}
				out.push_back( std::move( check ) );
			}

		private:
			// The element's text pattern, or its nearest ancestor's.
			[[nodiscard]] Com<IUIAutomationTextPattern> textPatternOf( Com<IUIAutomationElement> element ) const
			{
				for ( int depth = 0; depth < pattern_depth && element; ++depth )
				{
					Com<IUIAutomationTextPattern> pattern;
					if ( SUCCEEDED( element->GetCurrentPatternAs( UIA_TextPatternId, __uuidof( IUIAutomationTextPattern ), pattern.putVoid() ) ) && pattern )
					{
						return pattern;
					}
					Com<IUIAutomationElement> parent;
					if ( !walker_ || FAILED( walker_->GetParentElement( element.get(), parent.put() ) ) )
					{
						break;
					}
					element = std::move( parent );
				}
				return {};
			}

			// The sentence around the lookup (or at least its paragraph), for Anki notes.
			static void sentenceOf( IUIAutomationTextRange* character, CapturedText& captured )
			{
				for ( const auto unit : { TextUnit_Paragraph, TextUnit_Line } )
				{
					Com<IUIAutomationTextRange> paragraph;
					Com<IUIAutomationTextRange> before;
					if ( FAILED( character->Clone( paragraph.put() ) ) || !paragraph || FAILED( paragraph->ExpandToEnclosingUnit( unit ) ) || FAILED( paragraph->Clone( before.put() ) ) || !before )
					{
						continue;
					}
					// From the paragraph's start up to the looked-up text, to find where it is in the paragraph.
					if ( FAILED( before->MoveEndpointByRange( TextPatternRangeEndpoint_End, character, TextPatternRangeEndpoint_Start ) ) )
					{
						continue;
					}
					Bstr whole;
					Bstr head;
					if ( FAILED( paragraph->GetText( paragraph_limit, whole.put() ) ) || FAILED( before->GetText( paragraph_limit, head.put() ) ) )
					{
						continue;
					}
					const std::string text   = whole.utf8();
					const std::string prefix = head.utf8();
					if ( !text.starts_with( prefix ) || prefix.size() >= text.size() )
					{
						continue;
					}
					auto [sentence, offset]  = sentenceAround( text, prefix.size() );
					captured.sentence        = std::move( sentence );
					captured.sentence_offset = offset;
					return;
				}
			}

			bool                         com_ = false;
			Com<IUIAutomation>           automation_;
			Com<IUIAutomationTreeWalker> walker_;
		};

	} // namespace

	std::unique_ptr<TextCapture> createUiaCapture()
	{
		return std::make_unique<UiaCapture>();
	}

} // namespace lexiglance::platform
