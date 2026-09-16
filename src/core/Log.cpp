#include <lexiglance/core/Log.h>

#include <lexiglance/core/Thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <system_error>

namespace lexiglance::log
{

	namespace
	{

		std::atomic<Level>& levelStorage()
		{
			static std::atomic<Level> value{ Level::Info };
			return value;
		}

		// Warnings and errors kept for health reports.
		constexpr std::size_t kept_problems = 32;
		// A log this size is moved aside, so a run always starts with room and the one before it is still there.
		constexpr std::uintmax_t rotate_at = 2U << 20U;

		struct Sinks
		{
			std::mutex        mutex;
			std::ofstream     file;
			std::deque<Entry> problems;
		};

		Sinks& sinks()
		{
			static Sinks instance;
			return instance;
		}

		// This computer's time zone, looked up once; null where the system has no time zone database, and then times
		// are written in UTC rather than not at all.
		const std::chrono::time_zone* localZone() noexcept
		{
			static const std::chrono::time_zone* zone = [] -> const std::chrono::time_zone* {
				try
				{
					return std::chrono::current_zone();
				}
				catch ( ... )
				{
					return nullptr;
				}
			}();
			return zone;
		}

	} // namespace

	void setLevel( Level lvl ) noexcept
	{
		levelStorage().store( lvl, std::memory_order_relaxed );
	}

	Level level() noexcept
	{
		return levelStorage().load( std::memory_order_relaxed );
	}

	void setFile( const std::filesystem::path& path )
	{
		auto&                  s = sinks();
		const std::scoped_lock lock( s.mutex );
		// Closed first: Windows renames no file that is open, and the rotation below is a rename.
		s.file.close();

		if ( !path.empty() )
		{
			std::error_code ec;
			std::filesystem::create_directories( path.parent_path(), ec );
			if ( std::filesystem::file_size( path, ec ) > rotate_at )
			{
				auto previous = path;
				previous += ".old";
				std::filesystem::rename( path, previous, ec );
			}
			s.file.open( path, std::ios::out | std::ios::app );
		}
	}

	std::string timestamp( std::chrono::system_clock::time_point when )
	{
		const auto moment = std::chrono::floor<std::chrono::milliseconds>( when );
		if ( const auto* zone = localZone(); zone != nullptr )
		{
			return std::format( "{:%Y-%m-%d %H:%M:%S}", std::chrono::zoned_time( zone, moment ) );
		}
		return std::format( "{:%Y-%m-%d %H:%M:%S}", moment );
	}

	std::string day( std::chrono::system_clock::time_point when )
	{
		return timestamp( when ).substr( 0, 10 );
	}

	std::string_view levelName( Level lvl ) noexcept
	{
		switch ( lvl )
		{
			case Level::Trace:
				return "trace";
			case Level::Debug:
				return "debug";
			case Level::Info:
				return "info";
			case Level::Warn:
				return "warn";
			case Level::Error:
				return "error";
			case Level::Off:
				return "off";
		}
		return "?";
	}

	std::optional<Level> parseLevel( std::string_view name ) noexcept
	{
		for ( const Level lvl : { Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off } )
		{
			if ( name == levelName( lvl ) )
			{
				return lvl;
			}
		}
		return std::nullopt;
	}

	void write( Level lvl, std::string_view message )
	{
		const auto now  = std::chrono::floor<std::chrono::milliseconds>( std::chrono::system_clock::now() );
		const auto when = timestamp( now );
		// The thread as well, where it has a name: the daemon reads the screen, looks words up and draws popups at the
		// same time, and a log that does not say which is which cannot be followed.
		const auto thread = thread::name();
		const auto line   = thread.empty() ? std::format( "[{}] [{}] {}\n", when, levelName( lvl ), message ) : std::format( "[{}] [{}] [{}] {}\n", when, levelName( lvl ), thread, message );

		auto&                  s = sinks();
		const std::scoped_lock lock( s.mutex );
		( void )std::fwrite( line.data(), 1, line.size(), stderr );

		if ( s.file.is_open() )
		{
			s.file << line;
			s.file.flush();
		}
		if ( lvl >= Level::Warn && lvl != Level::Off )
		{
			if ( s.problems.size() == kept_problems )
			{
				s.problems.pop_front();
			}
			s.problems.push_back( { .time = now, .level = lvl, .message = std::string( message ) } );
		}
	}

	std::vector<Entry> recentProblems()
	{
		auto&                  s = sinks();
		const std::scoped_lock lock( s.mutex );
		return { s.problems.begin(), s.problems.end() };
	}

} // namespace lexiglance::log
