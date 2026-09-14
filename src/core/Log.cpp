#include <lexiglance/core/Log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

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
		s.file.close();

		if ( !path.empty() )
		{
			std::error_code ec;
			std::filesystem::create_directories( path.parent_path(), ec );
			s.file.open( path, std::ios::out | std::ios::app );
		}
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
		const auto line = std::format( "[{:%Y-%m-%d %H:%M:%S}] [{}] {}\n", now, levelName( lvl ), message );

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
