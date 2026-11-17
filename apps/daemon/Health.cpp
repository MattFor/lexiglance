#include "Anki.h"
#include "Audio.h"
#include "Daemon.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/dictionary/Classify.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
	#include <io.h>
	#include <process.h>
#else
	#include <unistd.h>
#endif

// The health report: every part a lookup depends on, checked the way it is used, each with what fixes it.
namespace lexiglance::daemon
{

	namespace
	{

		using Clock = std::chrono::steady_clock;
		using health::Check;
		using health::Severity;

		std::string readableDuration( std::chrono::seconds time )
		{
			const auto s = time.count();
			if ( s < 60 )
			{
				return std::format( "{} s", s );
			}
			if ( s < 3600 )
			{
				return std::format( "{} min", s / 60 );
			}
			if ( s < 86400 )
			{
				return std::format( "{} h {} min", s / 3600, ( s % 3600 ) / 60 );
			}
			return std::format( "{} days {} h", s / 86400, ( s % 86400 ) / 3600 );
		}

		std::chrono::milliseconds busyFor( const std::atomic<Clock::rep>& since )
		{
			const auto started = since.load( std::memory_order_relaxed );
			if ( started == 0 )
			{
				return std::chrono::milliseconds( 0 );
			}
			return std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - Clock::time_point( Clock::duration( started ) ) );
		}

		// A pipeline thread's answer: nullopt when it did not come in time. `replaced` tells when the request was dropped
		// for a newer one (the mailboxes hold one request), so it can simply be asked again.
		template <typename T>
		std::optional<T> await( std::future<T>& future, std::chrono::milliseconds timeout, bool* replaced = nullptr )
		{
			if ( future.wait_for( timeout ) != std::future_status::ready )
			{
				return std::nullopt;
			}
			try
			{
				return future.get();
			}
			catch ( const std::future_error& )
			{
				if ( replaced != nullptr )
				{
					*replaced = true;
				}
				return std::nullopt;
			}
		}

		std::string joined( const auto& items, std::string_view separator )
		{
			std::string out;
			for ( const auto& item : items )
			{
				if ( !out.empty() )
				{
					out.append( separator );
				}
				out.append( std::format( "{}", item ) );
			}
			return out;
		}

		bool directoryWritable( const std::filesystem::path& directory )
		{
			std::error_code ec;
			std::filesystem::create_directories( directory, ec );
#ifdef _WIN32
			return ::_waccess( directory.c_str(), 2 ) == 0;
#else
			return ::access( directory.c_str(), W_OK ) == 0;
#endif
		}

		bool recommendationInstalled( const lang::Recommendation& item, const std::vector<std::string>& titles )
		{
			return std::ranges::any_of( titles, [&]( const std::string& title ) {
				return ( !item.title.empty() && title == item.title ) || ( !item.title_prefix.empty() && title.starts_with( item.title_prefix ) );
			} );
		}

		void appendEnabledLanguages( std::vector<health::Check>& checks, const config::Config& cfg )
		{
			Check                    check{ .id = "languages", .title = "Languages" };
			const auto               enabled = lang::enabledLanguages( cfg.disabled_languages );
			std::vector<std::string> names;
			names.reserve( enabled.size() );
			for ( const lang::Language* language : enabled )
			{
				names.emplace_back( language->name() );
			}
			if ( enabled.empty() )
			{
				check.status = Severity::Error;
				check.detail = "All languages are turned off, so Lexiglance can't recognise any text. Turn at least one on in Scanning.";
				check.fix    = "open-scanning";
			}
			else
			{
				check.detail = std::format( "{} on: {}.", names.size() == 1 ? "One language" : std::format( "{} languages", names.size() ), joined( names, ", " ) );
				if ( !cfg.language.empty() && std::ranges::none_of( enabled, [&]( const lang::Language* language ) { return language->code() == cfg.language; } ) )
				{
					check.status = Severity::Warning;
					if ( const lang::Language* preferred = lang::findLanguage( cfg.language ) )
					{
						check.detail += std::format( " The preferred language, {}, is turned off.", preferred->name() );
					}
					else
					{
						check.detail += std::format( " The preferred language code \"{}\" is unknown or turned off.", cfg.language );
					}
					check.fix = "open-scanning";
				}
			}
			checks.push_back( std::move( check ) );
		}

	} // namespace

	void Daemon::checkDaemon( std::vector<health::Check>& checks, const config::Config& cfg, bool interactive ) const
	{
		// Which daemon this is, and whether it still is the program on disk.
		{
			Check      check{ .id = "daemon", .title = "Lexiglance" };
			const auto uptime = std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - started_ );
#ifdef _WIN32
			const int pid = _getpid();
#else
			const int pid = ::getpid();
#endif
			check.detail = std::format( "Version {}, pid {}, running for {} ({}).", version, pid, readableDuration( uptime ), process::executable().string() );
			if ( process::executableReplaced() )
			{
				check.status = Severity::Warning;
				check.detail += " The program changed on disk after it started (an update or a rebuild). Restart it to run the new version.";
				check.fix = "restart";
			}
			checks.push_back( std::move( check ) );
		}

		// Other daemons (another copy's, with a LEXIGLANCE_HOME of its own) show popups of their own. This one's home has
		// only this one, so restarting does not touch them.
		if ( const auto others = process::othersNamed( "lexiglanced" ); !others.empty() )
		{
			checks.push_back(
					{ .id     = "instances",
					  .title  = "Other copies of Lexiglance",
					  .status = Severity::Warning,
					  .detail = std::format(
							  "{} more {} running (pid {}), with {} own settings and popups. Quit any you don't use.",
							  others.size(),
							  others.size() == 1 ? "copy of Lexiglance is" : "copies of Lexiglance are",
							  joined( others, ", " ),
							  others.size() == 1 ? "its" : "their"
					  ) }
			);
		}

		{
			const std::scoped_lock lock( problems_mutex_ );
			for ( const auto& problem : startup_problems_ )
			{
				checks.push_back( { .id = "startup", .title = "Start-up", .status = Severity::Error, .detail = problem } );
			}
		}

		if ( cfg.paused )
		{
			checks.push_back(
					{ .id = "paused", .title = "Scanning", .status = Severity::Warning, .detail = "Scanning is paused, so holding the trigger does nothing.", .fix = "resume" }
			);
		}

		// The desktop: the UI thread answers and checks the trigger keys and input events itself.
		if ( !backend_ )
		{
#ifdef _WIN32
			constexpr std::string_view desktop = "the Windows desktop";
#elifdef __APPLE__
			constexpr std::string_view desktop = "the macOS desktop";
#else
			constexpr std::string_view desktop = "the desktop (X11)";
#endif
			checks.push_back(
					{ .id     = "desktop",
					  .title  = "Desktop",
					  .status = Severity::Error,
					  .detail = std::format( "Lexiglance isn't connected to {}, so the trigger and popups can't work. It may have been started outside your desktop session: restart it.", desktop ),
					  .fix    = "restart" }
			);
		}
		else
		{
			auto done   = std::make_shared<std::promise<std::vector<Check>>>();
			auto future = done->get_future();
			backend_->post( [this, done, interactive] {
				std::vector<Check> desktop;
				backend_->diagnose( desktop, interactive );
				done->set_value( std::move( desktop ) );
			} );
			if ( auto desktop = await( future, std::chrono::seconds( 2 ) ) )
			{
#ifdef _WIN32
				// Set up but not in effect (the Start-up item says why): setting it up again is the fix, not turning it on.
				if ( run_as_administrator_ && !elevated_ )
				{
					for ( Check& check : *desktop )
					{
						if ( check.id == "elevated" )
						{
							check.detail += " Run as administrator is already on but didn't take effect (see Start-up): click Run as administrator to set it up again.";
						}
					}
				}
#endif
				std::ranges::move( *desktop, std::back_inserter( checks ) );
			}
			else
			{
				checks.push_back(
						{ .id     = "desktop",
						  .title  = "Desktop",
						  .status = Severity::Error,
						  .detail = "The desktop thread isn't responding, so no popups can be shown. Restart Lexiglance.",
						  .fix    = "restart" }
				);
			}
		}
	}

	void Daemon::checkLanguages( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		checkDictionaries( checks, cfg );
		appendEnabledLanguages( checks, cfg );
		checkRecommendedDictionaries( checks, cfg );
		checkFonts( checks, cfg );
	}

	void Daemon::checkDictionaries( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		const auto  languages = languagesInUse( cfg );
		const auto  set       = dictionaries_.load();
		std::size_t words     = 0;
		std::size_t kanji     = 0;
		std::size_t meta      = 0;
		for ( const auto& loaded : set->all() )
		{
			words += loaded.dictionary->terms().empty() ? 0 : 1;
			kanji += loaded.dictionary->kanji().empty() ? 0 : 1;
			meta += loaded.dictionary->meta().empty() ? 0 : 1;
		}
		Check check{ .id = "dictionaries", .title = "Dictionaries" };
		if ( words == 0 )
		{
			check.status = Severity::Error;
			check.detail = set->all().empty() ? "No dictionary is enabled, so nothing can be looked up. Install or enable one on the Dictionaries page."
			                                  : "Only kanji dictionaries or frequency lists are enabled, so words can't be looked up. Install or enable a word dictionary.";
			check.fix    = "open-dictionaries";
		}
		else
		{
			check.detail = std::format( "{} enabled: {} with words, {} with kanji, {} with frequencies or pitch accents.", set->all().size(), words, kanji, meta );
		}
		{
			const std::scoped_lock lock( problems_mutex_ );
			if ( !load_errors_.empty() )
			{
				check.status = std::max( check.status, Severity::Warning );
				check.detail += std::format( " {} could not be opened: {}", load_errors_.size(), joined( std::span( load_errors_ ).first( std::min<std::size_t>( 2, load_errors_.size() ) ), "; " ) );
				check.fix = "open-dictionaries";
			}
		}
		checks.push_back( std::move( check ) );

		if ( words == 0 )
		{
			return;
		}

		// Not ipc_translator_: the check runs on a worker, beside the IPC thread.
		lookup::Translator       translator;
		Check                    lookup{ .id = "lookup", .title = "Looking words up" };
		const auto               started = Clock::now();
		std::size_t              found   = 0;
		std::vector<std::string> tried;
		tried.reserve( languages.size() * 4 );
		for ( const lang::Language* language : languages )
		{
			for ( const std::string_view word : language->sampleWords() )
			{
				found += translator.lookup( set, word, lookupOptions( cfg ) ).terms.empty() ? 0 : 1;
				tried.emplace_back( word );
			}
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>( Clock::now() - started ).count();
		if ( found == 0 )
		{
			lookup.status = Severity::Warning;
			lookup.detail = std::format( "None of these everyday words is in the enabled dictionaries: {}. Check that the right dictionaries are enabled.", joined( tried, ", " ) );
			lookup.fix    = "open-dictionaries";
		}
		else
		{
			lookup.detail = std::format( "Everyday words are found ({} µs for {} lookups).", elapsed, tried.size() );
		}
		checks.push_back( std::move( lookup ) );
	}

	void Daemon::checkRecommendedDictionaries( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		const lang::Language* preferred = lang::findLanguage( cfg.language );
		const auto            enabled   = lang::enabledLanguages( cfg.disabled_languages );
		if ( preferred == nullptr || !std::ranges::contains( enabled, preferred ) )
		{
			preferred = enabled.empty() ? nullptr : enabled.front();
		}
		if ( preferred == nullptr || preferred->recommendedDictionaries().empty() )
		{
			return;
		}

		std::vector<std::string> titles;
		if ( const auto installed = installed_.load() )
		{
			titles.reserve( installed->size() );
			for ( const auto& dictionary : *installed )
			{
				titles.push_back( dictionary->info().title );
			}
		}
		std::vector<std::string> missing;
		std::size_t              offered = 0;
		for ( const lang::Recommendation& item : preferred->recommendedDictionaries() )
		{
			if ( item.download.empty() )
			{
				continue;
			}
			++offered;
			if ( !recommendationInstalled( item, titles ) )
			{
				missing.push_back( item.name.empty() ? item.title : item.name );
			}
		}
		if ( offered > 0 && missing.empty() )
		{
			checks.push_back(
					{ .id = "recommended-dictionaries", .title = "Recommended dictionaries", .detail = std::format( "The recommended dictionaries for {} are installed.", preferred->name() ) }
			);
		}
		else if ( !missing.empty() )
		{
			const bool none = missing.size() == offered;
			checks.push_back(
					{ .id     = "recommended-dictionaries",
					  .title  = "Recommended dictionaries",
					  .status = none ? Severity::Warning : Severity::Info,
					  .detail = none ? std::format(
											   "None of the recommended dictionaries for {} are installed ({}). Get them on the Dictionaries page.",
											   preferred->name(),
											   joined( missing, ", " )
									   )
					                 : std::format( "Still missing for {}: {}. Get them on the Dictionaries page.", preferred->name(), joined( missing, ", " ) ),
					  .fix    = "open-dictionaries" }
			);
		}
	}

	void Daemon::checkFonts( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		const auto               languages = languagesInUse( cfg );
		Check                    check{ .id = "font", .title = "Fonts" };
		std::vector<std::string> drawn;
		std::vector<std::string> missing;
		std::vector<std::string> borrowed;
		drawn.reserve( languages.size() );
		for ( const lang::Language* language : languages )
		{
			const auto coverage = render::checkFont( language->sampleText(), cfg.popup.font_family );
			if ( coverage.missing > 0 )
			{
				missing.emplace_back( language->name() );
				continue;
			}
			drawn.push_back( std::format( "{} with {}", language->name(), coverage.family.empty() ? std::string( "the default font" ) : coverage.family ) );
			if ( !cfg.popup.font_family.empty() && !coverage.family.empty() && coverage.family != cfg.popup.font_family )
			{
				borrowed.emplace_back( language->name() );
			}
		}
		if ( !missing.empty() )
		{
			check.status = Severity::Error;
			check.detail = std::format( "No installed font has all the characters of {} text, so popups show boxes instead. Install one that does, such as Noto Sans (Noto Sans CJK JP for Japanese).", joined( missing, " and " ) );
		}
		else
		{
			check.detail = std::format( "Popups draw {}.", joined( drawn, "; " ) );
			if ( !borrowed.empty() )
			{
				check.status = Severity::Info;
				check.detail += std::format( " The chosen font, {}, has no {} characters, so another font fills in.", cfg.popup.font_family, joined( borrowed, " or " ) );
			}
		}
		checks.push_back( std::move( check ) );
	}

	void Daemon::checkScreen( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		// Reading the screen: accessibility, OCR, and the threads doing it.
		if ( backend_ )
		{
			if ( !cfg.scan.accessibility )
			{
				checks.push_back(
						{ .id     = "accessibility-setting",
						  .title  = "Accessibility for Qt and Chromium",
						  .status = Severity::Warning,
						  .detail = "Qt, Chromium and Electron applications aren't asked to expose their text, so only OCR can read it, which is slower and less exact.",
						  .fix    = "enable-accessibility" }
				);
			}

			if ( const auto busy = busyFor( capture_busy_since_ ); busy > std::chrono::seconds( 3 ) )
			{
				checks.push_back(
						{ .id     = "capture",
						  .title  = "Text capture",
						  .status = Severity::Error,
						  .detail = std::format( "Reading the text under the pointer has been stuck for {} s, most likely waiting on an application that isn't responding. Restart Lexiglance.", busy.count() / 1000 ),
						  .fix    = "restart" }
				);
			}
			else
			{
				std::optional<std::vector<Check>> capture;
				bool                              replaced = false;
				for ( int attempt = 0; attempt < 3 && !capture; ++attempt )
				{
					replaced    = false;
					auto future = [this] {
						auto promise = std::make_shared<std::promise<std::vector<Check>>>();
						auto result  = promise->get_future();
						// Not kept here: a newer request taking the mailbox's place breaks the promise, and it is asked again.
						capture_requests_.post( { .diagnose = std::move( promise ) } );
						return result;
					}();
					capture = await( future, std::chrono::seconds( 8 ), &replaced );
					if ( !replaced )
					{
						break;
					}
				}
				if ( capture )
				{
					std::ranges::move( *capture, std::back_inserter( checks ) );
				}
				else
				{
					checks.push_back(
							{ .id     = "capture",
							  .title  = "Text capture",
							  .status = replaced ? Severity::Warning : Severity::Error,
							  .detail = replaced ? "Text capture was busy with lookups. Check again without holding the trigger."
							                     : "Text capture didn't respond within 8 s. Restart Lexiglance.",
							  .fix    = replaced ? "" : "restart" }
					);
				}
			}

			if ( const auto busy = busyFor( render_busy_since_ ); busy > std::chrono::seconds( 3 ) )
			{
				checks.push_back(
						{ .id = "render", .title = "Drawing popups", .status = Severity::Error, .detail = std::format( "Drawing a popup has been stuck for {} s. Restart Lexiglance.", busy.count() / 1000 ), .fix = "restart" }
				);
			}

			// Worded to read after "Last lookup: " as well, which is where the settings application puts it.
			const std::scoped_lock lock( problems_mutex_ );
			if ( !last_capture_.empty() )
			{
				const auto ago = std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - last_capture_time_ );
				checks.push_back( { .id = "last-capture", .title = "Last lookup on screen", .status = Severity::Info, .detail = std::format( "{}, {} ago.", last_capture_, readableDuration( ago ) ) } );
			}
			else
			{
				checks.push_back(
						{ .id     = "last-capture",
						  .title  = "Last lookup on screen",
						  .status = Severity::Info,
						  .detail = "none since Lexiglance started: hold the trigger over a word to try one." }
				);
			}
		}
	}

	std::string Daemon::healthJson( bool interactive )
	{
		const auto         cfg = config();
		std::vector<Check> checks;
		// What is about to be looked at, so the settings application can say so instead of only "Checking...". Some of it
		// takes seconds (reading the screen, translating a test sentence).
		std::vector<std::string_view> steps{ "Lexiglance itself", "languages and dictionaries", "how the screen is read" };
		if ( translation_ )
		{
			steps.emplace_back( "how sentences are translated" );
		}
		if ( cfg->audio.enabled && !cfg->audio.sources.empty() )
		{
			steps.emplace_back( "audio" );
		}
		if ( cfg->anki.enabled )
		{
			steps.emplace_back( "Anki" );
		}
		steps.emplace_back( "settings and folders" );
		std::size_t done = 0;
		const auto  step = [&]( std::string_view what ) {
			json::Writer out;
			out.beginObject().field( "checking", what ).field( "done", static_cast<std::uint64_t>( ++done ) ).field( "total", static_cast<std::uint64_t>( steps.size() ) ).endObject();
			ipc_.broadcast( "health.progress", out.str() );
		};

		step( steps[0] );
		checkDaemon( checks, *cfg, interactive );
		step( steps[1] );
		checkLanguages( checks, *cfg );
		step( steps[2] );
		checkScreen( checks, *cfg );
		if ( translation_ )
		{
			step( "how sentences are translated" );
			auto translated = languagesInUse( *cfg );
			std::erase_if( translated, [&]( const lang::Language* language ) { return !cfg->translation.translates( language->code() ); } );
			translation_->diagnose( translated, cfg->translation.enabled, checks );
		}

		if ( cfg->audio.enabled && !cfg->audio.sources.empty() )
		{
			step( "audio" );
			Check check{ .id = "audio", .title = "Audio" };
#ifdef _WIN32
			check.detail = "Pronunciations play through Windows (MCI); clips in formats other than MP3 and WAV need ffplay or mpv on PATH.";
#else
			const auto player = audioPlayer();
			if ( player.empty() )
			{
				check.status = Severity::Warning;
				check.detail = "No audio player is installed, so pronunciations cannot play. Install ffmpeg (ffplay) or mpv.";
			}
			else
			{
				check.detail = std::format( "Pronunciations play with {}.", player );
			}
#endif
			checks.push_back( std::move( check ) );
		}

		if ( cfg->anki.enabled )
		{
			step( "Anki" );
			Check check{ .id = "anki", .title = "Anki" };
			if ( const auto anki = ankiVersion( cfg->anki ) )
			{
				check.detail = std::format( "AnkiConnect (version {}) answers at {}.", *anki, cfg->anki.url );
				if ( cfg->anki.deck.empty() || cfg->anki.model.empty() )
				{
					check.status = Severity::Warning;
					check.detail += " Choose a deck and a note type to add notes.";
					check.fix = "open-anki";
				}
			}
			else
			{
				check.status = Severity::Warning;
				check.detail = std::format( "Couldn't reach AnkiConnect at {} ({}). Start Anki with the AnkiConnect add-on, or turn off the \"add to Anki\" button.", cfg->anki.url, anki.error().message );
				check.fix    = "open-anki";
			}
			checks.push_back( std::move( check ) );
		}

		{
			step( "settings and folders" );
			Check check{ .id = "config", .title = "Settings file" };
			if ( !directoryWritable( config_path_.parent_path() ) )
			{
				check.status = Severity::Error;
				check.detail = std::format( "Lexiglance can't write to {}, so changed settings are lost when it restarts.", config_path_.parent_path().string() );
			}
			else
			{
				check.detail = std::format( "Saved in {}.", config_path_.string() );
			}
			checks.push_back( std::move( check ) );
		}

		{
			Check                                                            check{ .id = "folders", .title = "Data folders" };
			const std::vector<std::pair<const char*, std::filesystem::path>> folders{
				{ "dictionaries and OCR", paths::dataDir() },
				{ "logs", paths::stateDir() },
				{ "downloads", paths::cacheDir() },
			};
			std::vector<std::string> bad;
			std::vector<std::string> good;
			for ( const auto& [label, directory] : folders )
			{
				if ( directoryWritable( directory ) )
				{
					good.push_back( std::format( "{} ({})", label, directory.string() ) );
				}
				else
				{
					bad.push_back( std::format( "{} ({})", label, directory.string() ) );
				}
			}
			if ( !bad.empty() )
			{
				check.status = Severity::Error;
				check.detail = std::format( "Lexiglance can't write to {}, so it can't save dictionaries, logs or downloads there.", joined( bad, "; " ) );
			}
			else
			{
				check.detail = std::format( "Writable: {}.", joined( good, "; " ) );
			}
			checks.push_back( std::move( check ) );
		}

		// What went wrong lately, in the log's words.
		{
			const auto                     problems = log::recentProblems();
			const auto                     cutoff   = std::chrono::system_clock::now() - std::chrono::minutes( 30 );
			std::vector<const log::Entry*> recent;
			for ( const auto& entry : problems )
			{
				if ( entry.time >= cutoff )
				{
					recent.push_back( &entry );
				}
			}
			if ( !recent.empty() )
			{
				const bool  errors = std::ranges::any_of( recent, []( const log::Entry* entry ) { return entry->level == log::Level::Error; } );
				std::string latest;
				for ( auto it = recent.rbegin(); it != recent.rend() && std::distance( recent.rbegin(), it ) < 3; ++it )
				{
					latest.append( latest.empty() ? "" : "; " ).append( ( *it )->message );
				}
				checks.push_back(
						{ .id     = "log",
						  .title  = "Recent problems",
						  .status = errors ? Severity::Warning : Severity::Info,
						  .detail = std::format( "{} logged in the last 30 minutes. Latest: {}.", recent.size() == 1 ? std::string( "One problem" ) : std::format( "{} problems", recent.size() ), latest ) }
				);
			}
		}

		{
			const std::scoped_lock lock( problems_mutex_ );
			addSimulated( checks, simulated_ );
		}
		json::Writer out;
		health::write( out, checks );
		return out.take();
	}

} // namespace lexiglance::daemon
