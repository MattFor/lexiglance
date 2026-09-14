#include "Anki.h"
#include "Daemon.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/dictionary/Classify.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

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

#ifndef _WIN32
		// The first of `names` found on PATH.
		std::string findProgram( std::initializer_list<std::string_view> names )
		{
			const char* path = std::getenv( "PATH" );
			if ( path == nullptr )
			{
				return {};
			}
			for ( const auto name : names )
			{
				std::string_view directories( path );
				while ( !directories.empty() )
				{
					const auto                  colon     = directories.find( ':' );
					const std::filesystem::path candidate = std::filesystem::path( directories.substr( 0, colon ) ) / name;
					if ( ::access( candidate.c_str(), X_OK ) == 0 )
					{
						return std::string( name );
					}
					if ( colon == std::string_view::npos )
					{
						break;
					}
					directories.remove_prefix( colon + 1 );
				}
			}
			return {};
		}
#endif

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

	} // namespace

	void Daemon::checkDaemon( std::vector<health::Check>& checks, const config::Config& cfg, bool interactive ) const
	{
		// Which daemon this is, and whether it still is the program on disk.
		{
			Check      check{ .id = "daemon", .title = "Lexiglance daemon" };
			const auto uptime = std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - started_ );
			check.detail      = std::format( "Version {}, pid {}, running for {} ({}).", version, ::getpid(), readableDuration( uptime ), process::executable().string() );
			if ( process::executableReplaced() )
			{
				check.status = Severity::Warning;
				check.detail += " The program was rebuilt or updated after it started: restart it to run the new version.";
				check.fix = "restart";
			}
			checks.push_back( std::move( check ) );
		}

		// Other daemons (from before the instance lock, or ones that lost their socket) show popups of their own.
		if ( const auto others = process::othersNamed( "lexiglanced" ); !others.empty() )
		{
			checks.push_back(
					{ .id     = "instances",
			          .title  = "Other Lexiglance daemons",
			          .status = Severity::Error,
			          .detail = std::format(
							  "{} more {} running (pid {}). Each shows popups of its own, possibly with old settings; restarting stops them.",
							  others.size(),
							  others.size() == 1 ? "daemon is" : "daemons are",
							  joined( others, ", " )
					  ),
			          .fix = "restart" }
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
					{ .id = "paused", .title = "Scanning", .status = Severity::Warning, .detail = "Scanning is paused: holding the trigger does nothing until it is resumed.", .fix = "resume" }
			);
		}

		// The desktop: the UI thread answers and checks the trigger keys and input events itself.
		if ( !backend_ )
		{
			checks.push_back(
					{ .id     = "desktop",
			          .title  = "Desktop",
			          .status = Severity::Error,
			          .detail = "There is no connection to the desktop (X11), so the trigger and popups cannot work. Was the daemon started outside the graphical session?",
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
				std::ranges::move( *desktop, std::back_inserter( checks ) );
			}
			else
			{
				checks.push_back(
						{ .id     = "desktop",
				          .title  = "Desktop",
				          .status = Severity::Error,
				          .detail = "The desktop thread does not answer, so no popup can be shown.",
				          .fix    = "restart" }
				);
			}
		}
	}

	void Daemon::checkLanguages( std::vector<health::Check>& checks, const config::Config& cfg )
	{
		// The languages turned on that the enabled word dictionaries are in (all that are on, when none is known):
		// fonts and lookups are checked for these.
		const auto languages = languagesInUse( cfg );

		// Dictionaries, and a lookup through the same code as a popup's.
		{
			const auto  set   = dictionaries_.load();
			std::size_t words = 0;
			std::size_t kanji = 0;
			std::size_t meta  = 0;
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
				check.detail = set->all().empty() ? "No dictionary is installed and enabled, so nothing can be looked up."
				                                  : "No word dictionary is enabled (only kanji or frequency lists), so words cannot be looked up.";
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

			if ( words > 0 )
			{
				Check                    lookup{ .id = "lookup", .title = "Looking words up" };
				const auto               started = Clock::now();
				std::size_t              found   = 0;
				std::vector<std::string> tried;
				for ( const lang::Language* language : languages )
				{
					for ( const std::string_view word : language->sampleWords() )
					{
						found += ipc_translator_.lookup( set, word, lookupOptions( cfg ) ).terms.empty() ? 0 : 1;
						tried.emplace_back( word );
					}
				}
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>( Clock::now() - started ).count();
				if ( found == 0 )
				{
					lookup.status = Severity::Warning;
					lookup.detail = std::format( "None of {} is in the enabled dictionaries: are the right ones enabled?", joined( tried, ", " ) );
					lookup.fix    = "open-dictionaries";
				}
				else
				{
					lookup.detail = std::format( "Everyday words are found ({} µs for {} lookups).", elapsed, tried.size() );
				}
				checks.push_back( std::move( lookup ) );
			}
		}

		// Popups need glyphs for the languages of the dictionaries.
		{
			Check                    check{ .id = "font", .title = "Fonts" };
			std::vector<std::string> drawn;
			std::vector<std::string> missing;
			std::vector<std::string> borrowed;
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
				check.detail = std::format( "No installed font has every character of {} text, so popups show boxes for it. Install a font that has, such as Noto Sans (Noto Sans CJK JP for Japanese).", joined( missing, " and " ) );
			}
			else
			{
				check.detail = std::format( "Text is drawn: {}.", joined( drawn, "; " ) );
				if ( !borrowed.empty() )
				{
					check.status = Severity::Info;
					check.detail += std::format( " The chosen font, {}, has no {} of its own.", cfg.popup.font_family, joined( borrowed, " or " ) );
				}
			}
			checks.push_back( std::move( check ) );
		}
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
				          .detail = "Qt, Chromium and Electron applications are not asked to expose their text, so it can only be read with OCR (slower, less exact).",
				          .fix    = "enable-accessibility" }
				);
			}

			if ( const auto busy = busyFor( capture_busy_since_ ); busy > std::chrono::seconds( 3 ) )
			{
				checks.push_back(
						{ .id     = "capture",
				          .title  = "Text capture",
				          .status = Severity::Error,
				          .detail = std::format( "Reading the text under the pointer has been stuck for {} s, most likely in a call to an application that does not answer.", busy.count() / 1000 ),
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
					          .detail = replaced ? "Text capture was busy with lookups; check again without holding the trigger."
					                             : "Text capture did not answer within 8 s.",
					          .fix    = replaced ? "" : "restart" }
					);
				}
			}

			if ( const auto busy = busyFor( render_busy_since_ ); busy > std::chrono::seconds( 3 ) )
			{
				checks.push_back(
						{ .id = "render", .title = "Drawing popups", .status = Severity::Error, .detail = std::format( "Drawing a popup has been stuck for {} s.", busy.count() / 1000 ), .fix = "restart" }
				);
			}

			const std::scoped_lock lock( problems_mutex_ );
			if ( !last_capture_.empty() )
			{
				const auto ago = std::chrono::duration_cast<std::chrono::seconds>( Clock::now() - last_capture_time_ );
				checks.push_back( { .id = "last-capture", .title = "Last lookup on screen", .status = Severity::Info, .detail = std::format( "{} ago: {}.", readableDuration( ago ), last_capture_ ) } );
			}
			else
			{
				checks.push_back(
						{ .id     = "last-capture",
				          .title  = "Last lookup on screen",
				          .status = Severity::Info,
				          .detail = "None since the daemon started: hold the trigger over a word to try one." }
				);
			}
		}
	}

	std::string Daemon::healthJson( bool interactive )
	{
		const auto         cfg = config();
		std::vector<Check> checks;
		checkDaemon( checks, *cfg, interactive );
		checkLanguages( checks, *cfg );
		checkScreen( checks, *cfg );

		if ( cfg->audio.enabled && !cfg->audio.sources.empty() )
		{
			Check check{ .id = "audio", .title = "Audio" };
#ifdef _WIN32
			check.detail = "Pronunciations play through Windows (MCI); clips in formats other than MP3 and WAV need ffplay or mpv on PATH.";
#else
			const auto player = findProgram( { "ffplay", "mpv", "mpg123" } );
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
			Check check{ .id = "anki", .title = "Anki" };
			if ( const auto anki = ankiVersion( cfg->anki ) )
			{
				check.detail = std::format( "AnkiConnect (version {}) answers at {}.", *anki, cfg->anki.url );
				if ( cfg->anki.deck.empty() || cfg->anki.model.empty() )
				{
					check.status = Severity::Warning;
					check.detail += " Choose a deck and a note type so notes can be added.";
					check.fix = "open-anki";
				}
			}
			else
			{
				check.status = Severity::Warning;
				check.detail = std::format( "AnkiConnect does not answer at {} ({}). Start Anki with the AnkiConnect add-on, or turn Anki export off.", cfg->anki.url, anki.error().message );
				check.fix    = "open-anki";
			}
			checks.push_back( std::move( check ) );
		}

		{
			Check check{ .id = "config", .title = "Settings file" };
#ifdef _WIN32
			const bool writable = ::_waccess( config_path_.parent_path().c_str(), 2 ) == 0;
#else
			const bool writable = ::access( config_path_.parent_path().c_str(), W_OK ) == 0;
#endif
			if ( !writable )
			{
				check.status = Severity::Error;
				check.detail = std::format( "{} cannot be written, so changed settings are lost when Lexiglance restarts.", config_path_.parent_path().string() );
			}
			else
			{
				check.detail = std::format( "Saved in {}.", config_path_.string() );
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

		json::Writer out;
		health::write( out, checks );
		return out.take();
	}

} // namespace lexiglance::daemon
