#ifndef LEXIGLANCE_DAEMON_DAEMON_H
#define LEXIGLANCE_DAEMON_DAEMON_H

#include "Anki.h"
#include "Audio.h"
#include "IpcServer.h"
#include "Stats.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Health.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/dictionary/DictionaryStore.h>
#include <lexiglance/lookup/Translator.h>
#include <lexiglance/platform/Platform.h>
#include <lexiglance/render/PopupRenderer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lexiglance::daemon
{

	// Owns the lookup pipeline:
	//   UI thread (platform events) -> capture thread (text under cursor + lookup) -> render thread (Pango) -> UI thread
	// Each hand-over is a single-slot mailbox, so only the newest request is ever processed and nothing queues up.
	class Daemon
	{
	public:
		Daemon( std::unique_ptr<platform::Backend> backend, config::Config config );
		~Daemon();

		Daemon( const Daemon& )            = delete;
		Daemon& operator=( const Daemon& ) = delete;
		Daemon( Daemon&& )                 = delete;
		Daemon& operator=( Daemon&& )      = delete;

		int run();

		// Thread-safe.
		void requestQuit();

		// Something that went wrong before the daemon ran (e.g. an unreadable configuration), for the health report.
		void noteStartupProblem( std::string problem );

	private:
		struct CaptureRequest
		{
			std::uint64_t                                                        generation = 0;
			platform::Point                                                      point;
			platform::WindowInfo                                                 window;
			std::optional<std::string>                                           text;
			std::shared_ptr<std::promise<std::optional<platform::CapturedText>>> probe;
			// The capture's self-checks, for the health report.
			std::shared_ptr<std::promise<std::vector<health::Check>>> diagnose;
			bool                                                      refresh = false;
			// Look the latest capture up again with exactly this many characters (the wheel).
			int length = 0;
		};

		// The latest lookup from the screen, told to the settings application (throttled) and kept for the health report.
		struct CaptureReport
		{
			std::chrono::steady_clock::time_point at;
			std::string                           summary;
		};

		// What a popup shows, kept for its buttons.
		struct Shown
		{
			lookup::LookupResult result;
			std::string          sentence;
			std::size_t          sentence_offset = 0;
			// Characters captured (a length chosen with the wheel can go up to this).
			std::size_t available = 0;
		};

		struct RenderRequest
		{
			std::uint64_t                  generation = 0;
			std::shared_ptr<const Shown>   shown;
			platform::Rect                 anchor;
			std::optional<platform::Rect>  highlight;
			std::vector<render::NoteState> notes;
			std::uint64_t                  source = 0;
			std::uint64_t                  key    = 0;
		};

		// The popup on screen; UI thread only.
		struct CurrentPopup
		{
			std::uint64_t                  generation = 0;
			std::shared_ptr<const Shown>   shown;
			platform::Rect                 anchor;
			std::optional<platform::Rect>  highlight;
			std::vector<render::NoteState> notes;
			std::uint64_t                  source = 0;
		};

		struct ImportJob
		{
			std::uint64_t         id = 0;
			std::filesystem::path path;
			bool                  replace       = false;
			bool                  delete_source = false;
			std::string           replaces;
		};

		[[nodiscard]] std::shared_ptr<const config::Config> config() const
		{
			return config_.load();
		}

		Result<>                                   applyConfig( config::Config config, bool save );
		void                                       reloadDictionaries();
		[[nodiscard]] render::PopupStyle           styleFor( const config::Config& cfg ) const;
		[[nodiscard]] static lookup::LookupOptions lookupOptions( const config::Config& cfg );
		// The languages turned on that dictionaries are installed for (those turned on, when none is): what fonts and
		// lookups are checked for and OCR reads.
		[[nodiscard]] std::vector<const lang::Language*> languagesInUse( const config::Config& cfg ) const;
		// A word the dictionaries have, for previews and trials without a text of their own; IPC thread.
		[[nodiscard]] lookup::LookupResult sampleLookup( const std::shared_ptr<const lookup::DictionarySet>& set, const config::Config& cfg );
		// Writes what this daemon is working with (dictionaries, languages, what reads the screen, the popup) to the log.
		void logSetup( const config::Config& cfg );

		void               onScan( platform::Point point, const platform::WindowInfo& window );
		void               onClickOutside();
		void               onPopupAction( std::size_t entry, render::PopupAction action );
		void               onAdjustLength( int delta );
		void               playAudio( std::shared_ptr<const Shown> shown, std::size_t entry, bool report );
		void               addToAnki( std::shared_ptr<const Shown> shown, std::size_t entry );
		void               checkNotes( CurrentPopup popup );
		void               onSelection( std::string text, platform::Point point );
		void               dismiss();
		[[nodiscard]] bool current( std::uint64_t generation, std::uint64_t key ) const;
		[[nodiscard]] bool showing( std::uint64_t key, std::uint64_t posted_key, std::uint64_t posted_generation ) const;

		// The capture thread's text capture, rebuilt when its settings change or a new OCR model was installed.
		struct CaptureState
		{
			std::unique_ptr<platform::TextCapture> capture;
			std::string                            signature;
			std::uint64_t                          resets = 0;
			// When the capture wants its upkeep next (the accessibility bus must be taken in continually).
			std::optional<std::chrono::milliseconds> upkeep;
			// The latest capture from the screen, for changing its length with the wheel. It refers to the capture that
			// read it (its origin and handle), so it goes with it.
			std::optional<platform::CapturedText> last;
		};

		// What the capture thread read for a request. Where the text came from, and text that was read but is not looked
		// up, are for the capture report.
		struct Reading
		{
			std::optional<platform::CapturedText> captured;
			std::string_view                      source;
			std::string                           unread;
			std::chrono::milliseconds             took{ 0 };
		};

		void    refreshCapture( CaptureState& state, const config::Config& cfg );
		Reading readRequest( CaptureState& state, CaptureRequest& request, const config::Config& cfg );
		// Tallies a screen lookup for the Statistics page (in memory and, now and then, on disk).
		void recordScreenLookup( const CaptureRequest& request, const lookup::LookupResult& result, std::string_view source, std::string_view text );
		void captureLoop( const std::stop_token& stop );
		void renderLoop( const std::stop_token& stop );
		void importLoop( const std::stop_token& stop );
		void runImport( const ImportJob& job );

		void reportCapture( const CaptureRequest& request, std::string_view text, std::string_view unread, std::string_view source, std::chrono::milliseconds took, std::size_t entries, CaptureReport& reported );

		void registerHandlers();
		void registerActionHandlers();
		// Lookups, popups and previews, keys and diagnostics.
		void                      registerLookupHandlers();
		[[nodiscard]] std::string statusJson() const;
		// Runs every self-check (see Health.cpp); called on the IPC thread. `interactive`: started by a click.
		[[nodiscard]] std::string healthJson( bool interactive );
		// Parts of the health report: this daemon and the desktop; dictionaries, lookups and fonts of the languages in
		// use; reading the screen.
		void checkDaemon( std::vector<health::Check>& checks, const config::Config& cfg, bool interactive ) const;
		void checkLanguages( std::vector<health::Check>& checks, const config::Config& cfg );
		void checkScreen( std::vector<health::Check>& checks, const config::Config& cfg );

		std::unique_ptr<platform::Backend>                                                       backend_;
		dict::DictionaryStore                                                                    store_;
		std::filesystem::path                                                                    config_path_;
		std::atomic<std::shared_ptr<const config::Config>>                                       config_;
		std::atomic<std::shared_ptr<const lookup::DictionarySet>>                                dictionaries_;
		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<const dict::Dictionary>>>> installed_;
		std::mutex                                                                               reload_mutex_;
		// Codes of the languages of the enabled word dictionaries.
		std::atomic<std::shared_ptr<const std::vector<std::string>>> dictionary_languages_;

		std::atomic<std::uint64_t> generation_{ 0 };
		// What the popup on screen shows (0: nothing, or not from the screen), so identical redraws are skipped.
		std::atomic<std::uint64_t> shown_key_{ 0 };
		// The latest capture's key, and the generation the popup was last dismissed at: a popup on its way that shows what
		// the latest capture found stays valid although newer scans were made.
		std::atomic<std::uint64_t> latest_key_{ 0 };
		std::atomic<std::uint64_t> cancelled_{ 0 };
		std::atomic<std::uint64_t> lookups_{ 0 };
		std::atomic<std::uint64_t> lookup_nanoseconds_{ 0 };
		std::atomic<std::uint64_t> capture_resets_{ 0 };
		mutable std::mutex         capture_status_mutex_;
		std::string                capture_status_ = "starting";
		// Why a part of the capture is not working, in full; the status only summarises it.
		std::string                           capture_problem_;
		std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
		// Since when the capture and render threads work on their current item (steady clock ticks; 0 while idle): one
		// stuck in a call to a hung application shows in the health report.
		std::atomic<std::chrono::steady_clock::rep> capture_busy_since_{ 0 };
		std::atomic<std::chrono::steady_clock::rep> render_busy_since_{ 0 };
		mutable std::mutex                          problems_mutex_;
		std::vector<std::string>                    startup_problems_;
		std::vector<std::string>                    load_errors_;
		std::string                                 last_capture_;
		std::chrono::steady_clock::time_point       last_capture_time_;

		Mailbox<CaptureRequest> capture_requests_;
		Mailbox<RenderRequest>  render_requests_;

		std::mutex                  import_mutex_;
		std::condition_variable_any import_ready_;
		std::deque<ImportJob>       imports_;
		std::atomic<std::uint64_t>  next_job_{ 1 };

		std::mutex              quit_mutex_;
		std::condition_variable quit_signal_;
		bool                    quit_ = false;

		// The result behind the popup on screen, for its buttons; UI thread only.
		CurrentPopup current_;
		int          forced_length_ = 0;
		std::string  autoplayed_;
		AudioPlayer  audio_;
		// What has been looked up over time, for the Statistics page; kept between runs. `unsaved_stats_` counts the
		// lookups since it was last written (capture thread only).
		Statistics  stats_;
		std::size_t unsaved_stats_ = 0;
		// Network work behind the popup buttons (audio downloads, AnkiConnect).
		std::unique_ptr<ThreadPool> actions_;

		lookup::Translator                     ipc_translator_;
		std::unique_ptr<render::PopupRenderer> preview_renderer_;

		IpcServer    ipc_;
		std::jthread capture_thread_;
		std::jthread render_thread_;
		std::jthread import_thread_;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_DAEMON_H
