#ifndef LEXIGLANCE_CORE_LOG_H
#define LEXIGLANCE_CORE_LOG_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::log
{

	enum class Level : std::uint8_t
	{
		Trace,
		Debug,
		Info,
		Warn,
		Error,
		Off
	};

	void setLevel( Level lvl ) noexcept;

	[[nodiscard]] Level level() noexcept;

	[[nodiscard]] inline bool enabled( Level lvl ) noexcept
	{
		return lvl >= level();
	}

	// Mirrors every line into `path` in addition to stderr; an empty path disables the file sink.
	void setFile( const std::filesystem::path& path );

	void write( Level lvl, std::string_view message );

	// A moment as the clock on the wall shows it ("2026-09-16 20:14:30.123"), which is how log lines are stamped and
	// how the statistics count a day; UTC on a system without a time zone database.
	[[nodiscard]] std::string timestamp( std::chrono::system_clock::time_point when );

	// The date of a moment, locally ("2026-09-16").
	[[nodiscard]] std::string day( std::chrono::system_clock::time_point when );

	[[nodiscard]] std::optional<Level> parseLevel( std::string_view name ) noexcept;

	[[nodiscard]] std::string_view levelName( Level lvl ) noexcept;

	struct Entry
	{
		std::chrono::system_clock::time_point time;
		Level                                 level = Level::Info;
		std::string                           message;
	};

	// The latest warnings and errors written, oldest first, for health reports.
	[[nodiscard]] std::vector<Entry> recentProblems();

	template <typename... Args>
	void trace( std::format_string<Args...> format, Args&&... args )
	{
		if ( enabled( Level::Trace ) )
		{
			write( Level::Trace, std::format( format, std::forward<Args>( args )... ) );
		}
	}

	template <typename... Args>
	void debug( std::format_string<Args...> format, Args&&... args )
	{
		if ( enabled( Level::Debug ) )
		{
			write( Level::Debug, std::format( format, std::forward<Args>( args )... ) );
		}
	}

	template <typename... Args>
	void info( std::format_string<Args...> format, Args&&... args )
	{
		if ( enabled( Level::Info ) )
		{
			write( Level::Info, std::format( format, std::forward<Args>( args )... ) );
		}
	}

	template <typename... Args>
	void warn( std::format_string<Args...> format, Args&&... args )
	{
		if ( enabled( Level::Warn ) )
		{
			write( Level::Warn, std::format( format, std::forward<Args>( args )... ) );
		}
	}

	template <typename... Args>
	void error( std::format_string<Args...> format, Args&&... args )
	{
		if ( enabled( Level::Error ) )
		{
			write( Level::Error, std::format( format, std::forward<Args>( args )... ) );
		}
	}

} // namespace lexiglance::log

#endif // LEXIGLANCE_CORE_LOG_H
