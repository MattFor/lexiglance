#include "AtspiCapture.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Utf8.h>

#include <atspi/atspi.h>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <unordered_map>

namespace lexiglance::platform
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		// How long one capture may keep asking applications; a hung one otherwise stalls every lookup after it.
		constexpr auto capture_budget = std::chrono::milliseconds( 1200 );
		// Upkeep between captures: how often, and for how long at most each time.
		constexpr auto upkeep_interval = std::chrono::milliseconds( 200 );
		constexpr auto upkeep_budget   = std::chrono::milliseconds( 20 );

		template <typename T>
		struct Unref
		{
			void operator()( T* object ) const noexcept
			{
				if ( object != nullptr )
				{
					g_object_unref( object );
				}
			}
		};

		template <typename T>
		using Ref = std::unique_ptr<T, Unref<T>>;

		// Owns a GError out-parameter and clears it after every call.
		class ErrorSink
		{
		public:
			ErrorSink() = default;
			~ErrorSink()
			{
				clear();
			}

			ErrorSink( const ErrorSink& )            = delete;
			ErrorSink& operator=( const ErrorSink& ) = delete;
			ErrorSink( ErrorSink&& )                 = delete;
			ErrorSink& operator=( ErrorSink&& )      = delete;

			GError** out() noexcept
			{
				clear();
				return &error_;
			}

			[[nodiscard]] bool failed() const noexcept
			{
				return error_ != nullptr;
			}

			void clear() noexcept
			{
				g_clear_error( &error_ );
			}

		private:
			GError* error_ = nullptr;
		};

		std::optional<Rect> toRect( AtspiRect* box )
		{
			if ( box == nullptr )
			{
				return std::nullopt;
			}
			const Rect rect{ .x = box->x, .y = box->y, .width = box->width, .height = box->height };
			g_free( box );
			return rect.empty() ? std::nullopt : std::optional<Rect>( rect );
		}

		// Screen readers set org.a11y.Status.IsEnabled; toolkits such as Qt and Chromium only export their accessibility tree
		// once it is true.
		bool enableToolkitAccessibility()
		{
			ErrorSink                  error;
			const Ref<GDBusConnection> bus( g_bus_get_sync( G_BUS_TYPE_SESSION, nullptr, error.out() ) );
			if ( !bus )
			{
				log::warn( "accessibility: no session bus" );
				return false;
			}

			std::array<GVariant*, 3> arguments{
				g_variant_new_string( "org.a11y.Status" ),
				g_variant_new_string( "IsEnabled" ),
				g_variant_new_variant( g_variant_new_boolean( TRUE ) ),
			};
			GVariant* reply = g_dbus_connection_call_sync(
					bus.get(),
					"org.a11y.Bus",
					"/org/a11y/bus",
					"org.freedesktop.DBus.Properties",
					"Set",
					g_variant_new_tuple( arguments.data(), arguments.size() ),
					nullptr,
					G_DBUS_CALL_FLAGS_NONE,
					1000,
					nullptr,
					error.out()
			);
			if ( reply != nullptr )
			{
				g_variant_unref( reply );
				return true;
			}
			log::warn( "accessibility: cannot enable the accessibility bus" );
			return false;
		}

		class AtspiCapture final : public TextCapture
		{
		public:
			explicit AtspiCapture( bool enable_accessibility ) :
				toolkits_asked_( enable_accessibility )
			{
				if ( atspi_init() > 1 )
				{
					log::warn( "accessibility: AT-SPI initialisation failed" );
					return;
				}
				// A hung application must never stall scanning for long.
				atspi_set_timeout( 300, 3000 );
				if ( enable_accessibility )
				{
					toolkits_enabled_ = enableToolkitAccessibility();
				}
				desktop_.reset( atspi_get_desktop( 0 ) );
			}

			~AtspiCapture() override
			{
				pids_.clear();
				desktop_.reset();
			}

			AtspiCapture( const AtspiCapture& )            = delete;
			AtspiCapture& operator=( const AtspiCapture& ) = delete;
			AtspiCapture( AtspiCapture&& )                 = delete;
			AtspiCapture& operator=( AtspiCapture&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "at-spi";
			}

			std::optional<CapturedText> capture( Point point, const WindowInfo& window, CaptureScope scope ) override
			{
				const std::size_t max_chars = scope.characters;
				if ( !desktop_ )
				{
					return std::nullopt;
				}

				// Apply pending cache updates (children added, windows closed) delivered as D-Bus signals. Usually little is
				// left: idle() takes them in between captures.
				( void )drain( std::chrono::milliseconds( 30 ) );
				deadline_ = Clock::now() + capture_budget;

				auto target = windowAt( point, window.pid );
				if ( !target )
				{
					return std::nullopt;
				}

				// Descend to the deepest element under the pointer.
				ErrorSink error;
				for ( int depth = 0; depth < 64 && !expired(); ++depth )
				{
					const Ref<AtspiComponent> component( atspi_accessible_get_component_iface( target.get() ) );
					if ( !component )
					{
						break;
					}
					Ref<AtspiAccessible> child( atspi_component_get_accessible_at_point( component.get(), point.x, point.y, ATSPI_COORD_TYPE_SCREEN, error.out() ) );
					if ( !child || child.get() == target.get() )
					{
						break;
					}
					target = std::move( child );
				}

				Ref<AtspiText> text( atspi_accessible_get_text_iface( target.get() ) );
				if ( !text )
				{
					// GTK notebooks (tabbed editors and terminals) do not hit-test their pages.
					int budget = 256;
					if ( const auto found = searchText( target.get(), point, 0, budget ) )
					{
						text.reset( atspi_accessible_get_text_iface( found.get() ) );
					}
				}
				if ( !text )
				{
					return std::nullopt;
				}
				if ( expired() )
				{
					log::debug( "accessibility: gave up on the application under the pointer after {} ms", capture_budget.count() );
					return std::nullopt;
				}

				const int offset = atspi_text_get_offset_at_point( text.get(), point.x, point.y, ATSPI_COORD_TYPE_SCREEN, error.out() );
				if ( offset < 0 || error.failed() )
				{
					return std::nullopt;
				}

				// Toolkits report the nearest character even past the end of a line; require the pointer to be on it.
				const auto character = toRect( atspi_text_get_character_extents( text.get(), offset, ATSPI_COORD_TYPE_SCREEN, error.out() ) );
				if ( character )
				{
					const Rect slack{ .x = character->x - 2, .y = character->y - 2, .width = character->width + 4, .height = character->height + 4 };
					if ( !slack.contains( point ) )
					{
						return std::nullopt;
					}
				}

				gchar* content = atspi_text_get_text( text.get(), offset, offset + static_cast<gint>( max_chars ), error.out() );
				if ( content == nullptr )
				{
					return std::nullopt;
				}
				CapturedText captured{ .text = content, .offset = offset, .character = character.value_or( Rect{ .x = point.x, .y = point.y, .width = 1, .height = 1 } ) };
				g_free( content );

				// The sentence around the lookup (or at least its line), for Anki notes.
				for ( const auto granularity : { ATSPI_TEXT_GRANULARITY_SENTENCE, ATSPI_TEXT_GRANULARITY_LINE } )
				{
					AtspiTextRange* range = atspi_text_get_string_at_offset( text.get(), offset, granularity, error.out() );
					if ( range == nullptr )
					{
						continue;
					}
					if ( range->content != nullptr && range->start_offset <= offset && range->end_offset > offset )
					{
						const std::string_view whole( range->content );
						captured.sentence        = whole;
						captured.sentence_offset = utf8::prefix( whole, static_cast<std::size_t>( offset - range->start_offset ) ).size();
					}
					g_free( range->content );
					g_free( range );
					if ( !captured.sentence.empty() )
					{
						break;
					}
				}

				captured.handle = std::shared_ptr<void>( text.release(), []( void* object ) { g_object_unref( object ); } );
				return captured;
			}

			std::optional<Rect> bounds( const CapturedText& captured, std::size_t length ) override
			{
				auto* text = static_cast<AtspiText*>( captured.handle.get() );
				if ( text == nullptr || length == 0 )
				{
					return std::nullopt;
				}

				ErrorSink  error;
				const auto start = std::max( 0, captured.offset - static_cast<gint>( captured.rewound ) );
				const auto end   = start + static_cast<gint>( length );
				if ( auto range = toRect( atspi_text_get_range_extents( text, start, end, ATSPI_COORD_TYPE_SCREEN, error.out() ) ) )
				{
					return range;
				}

				// Some toolkits do not implement range extents: join the first and last character boxes.
				const auto first = toRect( atspi_text_get_character_extents( text, start, ATSPI_COORD_TYPE_SCREEN, error.out() ) );
				const auto last  = toRect( atspi_text_get_character_extents( text, end - 1, ATSPI_COORD_TYPE_SCREEN, error.out() ) );
				if ( !first || !last || last->y != first->y )
				{
					return first;
				}
				return Rect{ .x = first->x, .y = first->y, .width = last->x + last->width - first->x, .height = std::max( first->height, last->height ) };
			}

			// Messages from the accessibility bus are only taken in while its connection is dispatched. Left alone they
			// pile up, in the bus daemon and in our socket: every call then waits behind megabytes of events from busy
			// applications (browsers, Electron, Steam) and times out, and the cache of windows goes stale.
			std::optional<std::chrono::milliseconds> idle() override
			{
				if ( !desktop_ )
				{
					return std::nullopt;
				}
				return drain( upkeep_budget ) ? upkeep_interval : std::chrono::milliseconds( 5 );
			}

			void diagnose( std::vector<health::Check>& out ) override
			{
				health::Check check{ .id = "accessibility", .title = "Text in applications (accessibility)" };
				if ( !desktop_ )
				{
					check.status = health::Severity::Error;
					check.detail = "The accessibility bus (AT-SPI) cannot be reached, so text in ordinary applications is not read. Make sure at-spi2-core is "
								   "installed and that your session starts at-spi-bus-launcher, then restart Lexiglance.";
					check.fix    = "restart";
					out.push_back( std::move( check ) );
					return;
				}

				const bool kept_up = drain( std::chrono::milliseconds( 200 ) );
				ErrorSink  error;
				const auto started      = Clock::now();
				const int  applications = atspi_accessible_get_child_count( desktop_.get(), error.out() );
				const bool listed       = applications >= 0 && !error.failed();
				// A round trip through the bus daemon, which queues behind everything waiting for this connection.
				( void )atspi_accessible_get_process_id( desktop_.get(), error.out() );
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - started ).count();

				if ( !listed )
				{
					check.status = health::Severity::Error;
					check.detail = "The accessibility registry does not answer, so text in ordinary applications cannot be read. Restarting Lexiglance "
								   "reconnects to it.";
					check.fix    = "restart";
				}
				else
				{
					check.status = elapsed > 250 || !kept_up ? health::Severity::Warning : health::Severity::Ok;
					check.detail = std::format( "{} applications expose their text; the accessibility bus answered in {} ms ({} events handled).", applications, elapsed, dispatched_ );
					if ( !kept_up )
					{
						check.detail += " Applications are sending accessibility events faster than they can be taken in.";
					}
					else if ( elapsed > 250 )
					{
						check.detail += " That is slow: lookups in applications will lag.";
					}
				}
				if ( toolkits_asked_ && !toolkits_enabled_ )
				{
					check.detail += " Qt and Chromium applications could not be asked to expose their text (no session bus).";
				}
				out.push_back( std::move( check ) );
			}

		private:
			// Dispatches the accessibility bus's pending messages for at most `budget`; false when some are still waiting.
			bool drain( std::chrono::milliseconds budget )
			{
				const auto until = Clock::now() + budget;
				while ( g_main_context_iteration( nullptr, FALSE ) != 0 )
				{
					++dispatched_;
					if ( Clock::now() >= until )
					{
						return false;
					}
				}
				return true;
			}

			[[nodiscard]] bool expired() const
			{
				return Clock::now() >= deadline_;
			}

			std::uint32_t pidOf( AtspiAccessible* application )
			{
				if ( const auto it = pids_.find( application ); it != pids_.end() )
				{
					return it->second;
				}
				ErrorSink  error;
				const auto pid = static_cast<std::uint32_t>( atspi_accessible_get_process_id( application, error.out() ) );
				pids_.emplace( application, pid );
				return pid;
			}

			// The showing element with text under the point, found by walking the tree where hit-testing stops. Page tabs are
			// entered regardless of their extents, which cover only the tab label (or nothing).
			Ref<AtspiAccessible> searchText( AtspiAccessible* node, Point point, int depth, int& budget )
			{
				ErrorSink  error;
				const auto count = std::min( atspi_accessible_get_child_count( node, error.out() ), 64 );
				for ( int i = 0; i < count && budget > 0 && !expired(); ++i )
				{
					--budget;
					Ref<AtspiAccessible>     child( atspi_accessible_get_child_at_index( node, i, error.out() ) );
					const Ref<AtspiStateSet> states( child ? atspi_accessible_get_state_set( child.get() ) : nullptr );
					if ( !states || atspi_state_set_contains( states.get(), ATSPI_STATE_SHOWING ) == 0 )
					{
						continue;
					}
					const Ref<AtspiComponent> component( atspi_accessible_get_component_iface( child.get() ) );
					const bool                inside = component && atspi_component_contains( component.get(), point.x, point.y, ATSPI_COORD_TYPE_SCREEN, error.out() ) != 0;
					if ( !inside && atspi_accessible_get_role( child.get(), error.out() ) != ATSPI_ROLE_PAGE_TAB )
					{
						continue;
					}
					if ( inside && Ref<AtspiText>( atspi_accessible_get_text_iface( child.get() ) ) )
					{
						return child;
					}
					if ( depth < 12 )
					{
						if ( auto found = searchText( child.get(), point, depth + 1, budget ) )
						{
							return found;
						}
					}
				}
				return {};
			}

			// The showing top-level window containing the point, preferring the active one.
			Ref<AtspiAccessible> windowAt( Point point, std::uint32_t pid )
			{
				ErrorSink  error;
				const auto applications = atspi_accessible_get_child_count( desktop_.get(), error.out() );
				if ( applications != known_applications_ )
				{
					pids_.clear();
					known_applications_ = applications;
				}

				Ref<AtspiAccessible> best;
				bool                 best_active = false;
				for ( int i = 0; i < applications && !expired(); ++i )
				{
					const Ref<AtspiAccessible> application( atspi_accessible_get_child_at_index( desktop_.get(), i, error.out() ) );
					if ( !application || ( pid != 0 && pidOf( application.get() ) != pid ) )
					{
						continue;
					}

					const auto windows = atspi_accessible_get_child_count( application.get(), error.out() );
					for ( int j = 0; j < windows; ++j )
					{
						Ref<AtspiAccessible>     window( atspi_accessible_get_child_at_index( application.get(), j, error.out() ) );
						const Ref<AtspiStateSet> states( window ? atspi_accessible_get_state_set( window.get() ) : nullptr );
						if ( !states || atspi_state_set_contains( states.get(), ATSPI_STATE_SHOWING ) == 0 )
						{
							continue;
						}
						const Ref<AtspiComponent> component( atspi_accessible_get_component_iface( window.get() ) );
						if ( !component || atspi_component_contains( component.get(), point.x, point.y, ATSPI_COORD_TYPE_SCREEN, error.out() ) == 0 )
						{
							continue;
						}
						const bool active = atspi_state_set_contains( states.get(), ATSPI_STATE_ACTIVE ) != 0;
						if ( !best || ( active && !best_active ) )
						{
							best        = std::move( window );
							best_active = active;
						}
					}
				}
				return best;
			}

			Ref<AtspiAccessible>                                desktop_;
			std::unordered_map<AtspiAccessible*, std::uint32_t> pids_;
			int                                                 known_applications_ = -1;
			Clock::time_point                                   deadline_;
			std::uint64_t                                       dispatched_       = 0;
			bool                                                toolkits_asked_   = false;
			bool                                                toolkits_enabled_ = false;
		};

	} // namespace

	std::unique_ptr<TextCapture> createAtspiCapture( bool enable_accessibility )
	{
		return std::make_unique<AtspiCapture>( enable_accessibility );
	}

} // namespace lexiglance::platform
