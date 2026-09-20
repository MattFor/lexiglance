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
		// A translation shown with a lookup, of `characters` characters in `language`. `took` is how long the model worked
		// on it; zero when it was remembered from before.
		void recordTranslation( std::string_view language, std::size_t characters, std::chrono::microseconds took );

		// Whether lookups (and what is done with them) are counted, and whether translations are: the two ticks on the
		// Statistics page.
		void setEnabled( bool lookups, bool translations );

		// {lookups, found, words, days, ...}: the whole tally, as the Statistics page shows it.
		[[nodiscard]] std::string json() const;

		// Forgets everything counted so far and writes the empty tally.
		void reset();

	private:
		using Counts = std::map<std::string, std::uint64_t, std::less<>>;

		// What one day saw, for the Statistics page's chart and what it shows of a day.
		struct Day
		{
			std::uint64_t lookups               = 0;
			std::uint64_t found                 = 0;
			std::uint64_t characters            = 0;
			std::uint64_t popups                = 0;
			std::uint64_t audio                 = 0;
			std::uint64_t anki                  = 0;
			std::uint64_t translations          = 0;
			std::uint64_t translated_characters = 0;
		};

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
			// Translations shown, their characters, and the time the model took for those it made (not remembered ones).
			std::uint64_t translations             = 0;
			std::uint64_t translated_characters    = 0;
			std::uint64_t translations_made        = 0;
			std::uint64_t translation_microseconds = 0;
			std::string   first_day;
			Counts        languages;
			Counts        translated_languages;
			Counts        sources;
			Counts        words;
			// By day ("2026-09-19"), oldest first.
			std::map<std::string, Day, std::less<>> days;
		};

		// Today's entry, the oldest days dropped when there are too many; called with the lock held.
		Day& today();
		// Counts one of the totals and the same of today's; called with the lock held.
		void mark( std::uint64_t& counter, std::uint64_t Day::* daily, std::uint64_t by = 1 );

		mutable std::mutex    mutex_;
		std::filesystem::path file_;
		Tally                 tally_;
		bool                  enabled_              = true;
		bool                  translations_enabled_ = true;
		bool                  dirty_                = false;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_STATS_H
