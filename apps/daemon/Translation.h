#ifndef LEXIGLANCE_DAEMON_TRANSLATION_H
#define LEXIGLANCE_DAEMON_TRANSLATION_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/Health.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/translate/Model.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lexiglance::daemon
{

	// Sentences translated offline on a thread of their own: one at a time, and only the newest of those waiting (the
	// pointer has moved on from the others). Models load when first needed and are let go after a while unused, since
	// each holds several hundred megabytes; translations made are kept.
	class TranslationService
	{
	public:
		// Called on the translation thread when a request is done: the translation, or why there is none, and how long the
		// model worked on it.
		using Done = std::function<void( std::uint64_t ticket, Result<std::string>, std::chrono::microseconds took )>;

		explicit TranslationService( Done done );
		~TranslationService();

		TranslationService( const TranslationService& )            = delete;
		TranslationService& operator=( const TranslationService& ) = delete;
		TranslationService( TranslationService&& )                 = delete;
		TranslationService& operator=( TranslationService&& )      = delete;

		// A translation already made of `text`, if there is one.
		[[nodiscard]] std::optional<std::string> cached( const lang::Language& language, std::string_view text );

		// Translates `text`, written in `language`; a request still waiting is dropped for it.
		void request( std::uint64_t ticket, const lang::Language& language, std::string text );

		// Translates `text` and waits for it, at most `timeout` (lexiglancectl, the settings application): after the request
		// being worked on, before any waiting one.
		[[nodiscard]] Result<std::string> translateNow( const lang::Language& language, std::string text, std::chrono::seconds timeout );

		// Why text in `language` cannot be translated as things are (no model for it, or not downloaded), or empty.
		[[nodiscard]] static std::string unavailable( const lang::Language& language );

		// Models are loaded afresh when next needed (after a download, or one removed).
		void reload();

		// Which weights to load for each language, and for those with no choice of their own (Translation page): a change
		// lets go of the loaded models and of the translations made.
		void setPrecisions( std::map<std::string, translate::Precision> languages, translate::Precision others );

		// Which weights `language` is loaded from, as the settings ask; what is downloaded may be the other ones.
		[[nodiscard]] translate::Precision precisionFor( const lang::Language& language ) const;

		// Loads the model for `language` ahead of a translation likely to come (the sentence key went down), so the
		// translation does not wait for it. Nothing when it is loaded, or not downloaded.
		void warm( const lang::Language& language );

		// The health report's check: whether each language that has a model has it downloaded, and that it translates
		// the language's test sentence. Waits for the translation thread.
		void diagnose( std::span<const lang::Language* const> languages, bool enabled, std::vector<health::Check>& out );

	private:
		struct Request
		{
			std::uint64_t         ticket   = 0;
			const lang::Language* language = nullptr;
			std::string           text;
		};

		void run( const std::stop_token& stop );
		// The loaded model for `language`, loading it if need be (translation thread only).
		Result<translate::Model*> model( const lang::Language& language );
		void                      remember( const lang::Language& language, const std::string& text, const std::string& translation );

		Done                                        done_;
		mutable std::mutex                          mutex_;
		std::condition_variable_any                 wake_;
		std::optional<Request>                      pending_;
		std::deque<std::move_only_function<void()>> jobs_;
		bool                                        reload_ = false;
		// A warm() waiting to be done: the scans of a key held down ask again and again.
		bool warming_ = false;
		// A loaded model, which weights it came from (a language given the others is loaded afresh) and when it last
		// translated, so the models kept are those in use.
		struct Loaded
		{
			translate::Precision                  precision = translate::Precision::Compact;
			std::chrono::steady_clock::time_point used;
			std::unique_ptr<translate::Model>     model;
		};

		std::map<std::string, Loaded> models_;
		// The weights each language asks for, and those languages not named here use.
		std::map<std::string, translate::Precision> precisions_;
		translate::Precision                        others_ = translate::Precision::Compact;
		std::chrono::steady_clock::time_point       last_used_;
		// A translation made before: what it was asked for (language code and text), how often it has come back, and how
		// long it is kept.
		struct Remembered
		{
			std::string                           key;
			std::string                           text;
			std::uint32_t                         uses = 1;
			std::chrono::steady_clock::time_point until;
		};

		// Made translations, the most recent first.
		std::list<Remembered> cache_;
		std::jthread          thread_;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_TRANSLATION_H
