#ifndef LEXIGLANCE_DAEMON_AUDIO_H
#define LEXIGLANCE_DAEMON_AUDIO_H

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Error.h>
#include <lexiglance/language/Language.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#ifndef _WIN32
	#include <sys/types.h>
#endif

namespace lexiglance::daemon
{

	// Where a source has the pronunciation of a word of `language`: "jpod101" (JapanesePod101, Japanese words),
	// "commons" (Wikimedia Commons, languages with a Commons prefix) or a URL template with {term}, {reading} and
	// {language}. Empty when the source has no words of the language.
	[[nodiscard]] std::string audioUrl( std::string_view source, std::string_view expression, std::string_view reading, const lang::Language* language );

	// Term pronunciations from the configured sources, played with an external player (ffplay, mpv, mpg123, ...), on
	// Windows through MCI first. All methods block and run on worker threads.
	class AudioPlayer
	{
	public:
		struct Clip
		{
			std::string url;
			std::string data;
			// Hash of the source's "not available" placeholder, for AnkiConnect's skipHash.
			std::string placeholder_hash;
		};

		AudioPlayer() = default;
		~AudioPlayer();

		AudioPlayer( const AudioPlayer& )            = delete;
		AudioPlayer& operator=( const AudioPlayer& ) = delete;
		AudioPlayer( AudioPlayer&& )                 = delete;
		AudioPlayer& operator=( AudioPlayer&& )      = delete;

		// Tries the sources that have words of `language`, or Wikimedia Commons when none of them has.
		[[nodiscard]] Result<std::shared_ptr<const Clip>> find( std::string_view expression, std::string_view reading, const lang::Language* language, const config::AudioSettings& settings );

		Result<> play( std::string_view expression, std::string_view reading, const lang::Language* language, const config::AudioSettings& settings );

		void stop();

	private:
		std::mutex                                                   cache_mutex_;
		std::unordered_map<std::string, std::shared_ptr<const Clip>> cache_;
		std::mutex                                                   play_mutex_;
#ifdef _WIN32
		// The clip open in MCI, or the external player's process handle.
		bool  mci_open_ = false;
		void* player_   = nullptr;
#else
		pid_t player_ = -1;
#endif
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_AUDIO_H
