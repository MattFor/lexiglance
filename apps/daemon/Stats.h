#ifndef LEXIGLANCE_DAEMON_STATS_H
#define LEXIGLANCE_DAEMON_STATS_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace lexiglance::daemon
{

	// What Lexiglance has done over time: how much was looked up, in which languages, through what, which words came
	// back most often, and what was done with them. Kept in the state directory between runs and shown on the
	// Statistics page. It never leaves this computer, counting can be turned off, and it can be emptied at a click.
	class Statistics
	{
	public:
		// Reads the tally, or starts a new one when there is none. `file` is where save() writes.
		void load( std::filesystem::path file );

		// Writes the tally when it has changed since the last time; done every so often and when the daemon ends.
		void save();

		// One lookup of text read from the screen. `word` is the first entry found, empty when nothing matched.
		void recordLookup( std::string_view word, std::string_view language, std::string_view source, std::size_t characters, std::chrono::microseconds took );
		void recordPopup();
		void recordAudio();
		void recordAnki();

		// Whether counting happens at all (Settings -> statistics).
		void setEnabled( bool enabled );

		// {lookups, found, words, days, ...}: the whole tally, as the Statistics page shows it.
		[[nodiscard]] std::string json() const;

		// Forgets everything counted so far and writes the empty tally.
		void reset();

	private:
		using Counts = std::map<std::string, std::uint64_t, std::less<>>;

		// The tally as one value, so resetting and loading are a single assignment.
		struct Tally
		{
			std::uint64_t lookups      = 0;
			std::uint64_t found        = 0;
			std::uint64_t characters   = 0;
			std::uint64_t popups       = 0;
			std::uint64_t audio        = 0;
			std::uint64_t anki         = 0;
			std::uint64_t microseconds = 0;
			std::uint64_t sessions     = 0;
			std::string   first_day;
			Counts        languages;
			Counts        sources;
			Counts        words;
			Counts        days;
		};

		// Counts a day and keeps the maps from growing without end; called with the lock held.
		void mark( std::uint64_t& counter, std::uint64_t by = 1 );

		mutable std::mutex    mutex_;
		std::filesystem::path file_;
		Tally                 tally_;
		bool                  enabled_ = true;
		bool                  dirty_   = false;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_STATS_H
