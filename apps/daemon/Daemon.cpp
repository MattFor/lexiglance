#include "Daemon.h"

#include <lexiglance/core/Glob.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/dictionary/Classify.h>
#include <lexiglance/ipc/Messages.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>

#include <unistd.h>

namespace lexiglance::daemon
{

	namespace
	{

		std::string base64( std::string_view data )
		{
			static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string                       out;
			out.reserve( ( ( data.size() + 2 ) / 3 ) * 4 );
			for ( std::size_t i = 0; i < data.size(); i += 3 )
			{
				const auto          byte = [&]( std::size_t k ) { return k < data.size() ? static_cast<std::uint32_t>( static_cast<unsigned char>( data[k] ) ) : 0U; };
				const std::uint32_t n    = ( byte( i ) << 16U ) | ( byte( i + 1 ) << 8U ) | byte( i + 2 );
				out.push_back( alphabet[( n >> 18U ) & 63U] );
				out.push_back( alphabet[( n >> 12U ) & 63U] );
				out.push_back( i + 1 < data.size() ? alphabet[( n >> 6U ) & 63U] : '=' );
				out.push_back( i + 2 < data.size() ? alphabet[n & 63U] : '=' );
			}
			return out;
		}

		cairo_status_t appendPng( void* closure, const unsigned char* data, unsigned int length )
		{
			static_cast<std::string*>( closure )->append( reinterpret_cast<const char*>( data ), length );
			return CAIRO_STATUS_SUCCESS;
		}

		double desktopScale( const platform::Backend* backend )
		{
			return backend != nullptr ? backend->scaleFactor() : 1.0;
		}

		dict::Markup markupFor( std::string_view name )
		{
			if ( name == "plain" )
			{
				return dict::Markup::Plain;
			}
			if ( name == "pango" )
			{
				return dict::Markup::Pango;
			}
			return dict::Markup::Html;
		}

		std::string_view textOf( const lookup::LookupResult& result )
		{
			return result.matchedText( result.matched_length );
		}

		render::Scheme schemeOf( config::ColorScheme scheme )
		{
			switch ( scheme )
			{
				case config::ColorScheme::Paper:
					return render::Scheme::Paper;
				case config::ColorScheme::Nord:
					return render::Scheme::Nord;
				case config::ColorScheme::Sakura:
					return render::Scheme::Sakura;
				case config::ColorScheme::Matcha:
					return render::Scheme::Matcha;
				case config::ColorScheme::Midnight:
					return render::Scheme::Midnight;
				case config::ColorScheme::Contrast:
					return render::Scheme::Contrast;
				case config::ColorScheme::Default:
					break;
			}
			return render::Scheme::Default;
		}

		render::Design designOf( config::PopupDesign design )
		{
			switch ( design )
			{
				case config::PopupDesign::Classic:
					return render::Design::Classic;
				case config::PopupDesign::Compact:
					return render::Design::Compact;
				case config::PopupDesign::Friendly:
					break;
			}
			return render::Design::Friendly;
		}

		// Identifies what a popup from the screen shows, so an identical one is not drawn again; 0 for other popups.
		std::uint64_t popupKey( bool from_screen, const platform::CapturedText& captured, std::uint32_t matched, int length, const platform::Rect& anchor )
		{
			if ( !from_screen )
			{
				return 0;
			}
			return std::hash<std::string>{}( std::format( "{}|{}|{}|{},{},{},{}", captured.text, matched, length, anchor.x, anchor.y, anchor.width, anchor.height ) );
		}

		std::size_t characters( const std::optional<platform::CapturedText>& captured )
		{
			return captured ? utf8::length( captured->text ) : 0;
		}

		platform::Rect pointBox( platform::Point point )
		{
			return { .x = point.x, .y = point.y, .width = 1, .height = 16 };
		}

		std::string pngBase64( cairo_surface_t* surface )
		{
			std::string png;
			cairo_surface_write_to_png_stream( surface, &appendPng, &png );
			return base64( png );
		}

		// Marks a pipeline thread as working on an item, so one stuck in a call to a hung application shows in the health
		// report.
		class BusyMark
		{
		public:
			explicit BusyMark( std::atomic<std::chrono::steady_clock::rep>& since ) :
				since_( &since )
			{
				since_->store( std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed );
			}

			~BusyMark()
			{
				since_->store( 0, std::memory_order_relaxed );
			}

			BusyMark( const BusyMark& )            = delete;
			BusyMark& operator=( const BusyMark& ) = delete;
			BusyMark( BusyMark&& )                 = delete;
			BusyMark& operator=( BusyMark&& )      = delete;

		private:
			std::atomic<std::chrono::steady_clock::rep>* since_;
		};

		// The start of a text on one line, for messages.
		std::string excerpt( std::string_view text, std::size_t characters = 16 )
		{
			const auto  head = utf8::prefix( text, characters );
			std::string out( head );
			std::ranges::replace( out, '\n', ' ' );
			std::ranges::replace( out, '\t', ' ' );
			while ( !out.empty() && out.back() == ' ' )
			{
				out.pop_back();
			}
			return head.size() < text.size() ? out + "…" : out;
		}

		// Profiles of dictionaries in a priority list, in its order (titles not installed get an empty one).
		std::vector<dict::Profile> profilesOf( const std::vector<config::DictionaryPreference>& preferences, const std::vector<std::shared_ptr<const dict::Dictionary>>& installed )
		{
			std::vector<dict::Profile> profiles;
			profiles.reserve( preferences.size() );
			for ( const auto& preference : preferences )
			{
				const auto it = std::ranges::find_if( installed, [&]( const auto& d ) { return d->info().title == preference.title; } );
				profiles.push_back( it != installed.end() ? dict::profile( **it ) : dict::Profile{ .title = preference.title } );
			}
			return profiles;
		}

	} // namespace

	Daemon::Daemon( std::unique_ptr<platform::Backend> backend, config::Config config ) :
		backend_( std::move( backend ) ),
		store_( paths::dictionariesDir() ),
		config_path_( paths::configFile() ),
		config_( std::make_shared<const config::Config>( std::move( config ) ) )
	{
		actions_ = std::make_unique<ThreadPool>( 2, false, "lg-actions" );
		if ( lang::findLanguage( this->config()->language ) == nullptr )
		{
			log::warn( "unsupported language \"{}\": text is looked up in the language of its script", this->config()->language );
		}
	}

	Daemon::~Daemon()
	{
		// The socket goes first: a new daemon can take over at once, and no request reaches a half torn down one.
		ipc_.stop();
		actions_.reset();
		capture_thread_ = {};
		render_thread_  = {};
		import_thread_  = {};
	}

	void Daemon::noteStartupProblem( std::string problem )
	{
		const std::scoped_lock lock( problems_mutex_ );
		startup_problems_.push_back( std::move( problem ) );
	}

	void Daemon::requestQuit()
	{
		if ( backend_ )
		{
			backend_->quit();
		}
		{
			const std::scoped_lock lock( quit_mutex_ );
			quit_ = true;
		}
		quit_signal_.notify_all();
	}

	int Daemon::run()
	{
		reloadDictionaries();
		registerHandlers();

#ifndef _WIN32
		// The socket's directory (a Windows named pipe has none).
		if ( auto dir = paths::ensureDirectory( std::filesystem::path( paths::ipcEndpoint() ).parent_path(), true ); !dir )
		{
			log::error( "{}", dir.error().message );
			return 1;
		}
#endif
		if ( auto started = ipc_.start( paths::ipcEndpoint() ); !started )
		{
			log::error( "{}", started.error().message );
			return 1;
		}
		log::info( "listening on {}", paths::ipcEndpoint() );

		import_thread_ = std::jthread( [this]( const std::stop_token& stop ) { importLoop( stop ); } );

		if ( !backend_ )
		{
			log::warn( "no desktop integration: serving lookups and imports only" );
			std::unique_lock lock( quit_mutex_ );
			quit_signal_.wait( lock, [this] { return quit_; } );
			return 0;
		}

		platform::Events events;
		events.scan             = [this]( platform::Point point, const platform::WindowInfo& window ) { onScan( point, window ); };
		events.click_outside    = [this]( platform::Point ) { onClickOutside(); };
		events.selection        = [this]( std::string text, platform::Point point ) { onSelection( std::move( text ), point ); };
		events.trigger_released = [] {};
		// The settings application shows whether the trigger is seen (Overview, Health).
		events.trigger_changed = [this]( bool held ) { ipc_.broadcast( "trigger.changed", held ? R"({"held":true})" : R"({"held":false})" ); };
		events.popup_action    = [this]( std::size_t entry, render::PopupAction action ) { onPopupAction( entry, action ); };
		events.adjust_length   = [this]( int delta ) { onAdjustLength( delta ); };
		events.source_closed   = [this] { onClickOutside(); };
		if ( auto started = backend_->start( std::move( events ) ); !started )
		{
			log::error( "{}", started.error().message );
			return 1;
		}
		backend_->configure( *config() );

		capture_thread_ = std::jthread( [this]( const std::stop_token& stop ) { captureLoop( stop ); } );
		render_thread_  = std::jthread( [this]( const std::stop_token& stop ) { renderLoop( stop ); } );

		const auto cfg = config();
		log::info(
				"{} {} ready: backend {}, trigger {}, {} dictionaries",
				app_name,
				version,
				backend_->name(),
				cfg->scan.trigger.empty() ? "" : cfg->scan.trigger.front(),
				dictionaries_.load()->all().size()
		);
		backend_->run();
		log::info( "shutting down" );
		return 0;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Configuration and dictionaries
	// ---------------------------------------------------------------------------------------------------------------------

	Result<> Daemon::applyConfig( config::Config config, bool save )
	{
		if ( save )
		{
			if ( auto saved = config.save( config_path_ ); !saved )
			{
				return saved;
			}
		}
		// Only an actual change applies, so --verbose survives unrelated edits.
		const auto previous = this->config();
		if ( const auto level = log::parseLevel( config.log_level ); level && previous && previous->log_level != config.log_level )
		{
			log::setLevel( *level );
		}

		auto snapshot = std::make_shared<const config::Config>( std::move( config ) );
		config_.store( snapshot );
		capture_requests_.post( { .refresh = true } );
		if ( backend_ )
		{
			backend_->post( [this, snapshot] {
				backend_->configure( *snapshot );
				if ( snapshot->paused )
				{
					dismiss();
				}
			} );
		}
		reloadDictionaries();
		ipc_.broadcast( "config.changed", "{}" );
		return {};
	}

	void Daemon::reloadDictionaries()
	{
		const std::scoped_lock lock( reload_mutex_ );

		std::vector<Error> errors;
		auto               installed = std::make_shared<std::vector<std::shared_ptr<const dict::Dictionary>>>( store_.loadAll( &errors ) );
		{
			const std::scoped_lock problems_lock( problems_mutex_ );
			load_errors_.clear();
			for ( const auto& error : errors )
			{
				log::warn( "{}", error.message );
				load_errors_.push_back( error.message );
			}
		}

		// Keep the configured order; newly installed dictionaries go to the end, uninstalled ones are forgotten.
		auto cfg     = *config();
		bool changed = false;
		std::erase_if( cfg.dictionaries, [&]( const config::DictionaryPreference& preference ) {
			const bool missing = std::ranges::none_of( *installed, [&]( const auto& d ) { return d->info().title == preference.title; } );
			changed            = changed || missing;
			return missing;
		} );
		// A newly installed dictionary goes where it belongs (a word dictionary before names, kanji and frequency lists),
		// not simply last.
		if ( std::ranges::any_of( *installed, [&]( const auto& d ) { return cfg.dictionary( d->info().title ) == nullptr; } ) )
		{
			auto placed = profilesOf( cfg.dictionaries, *installed );
			for ( const auto& dictionary : *installed )
			{
				if ( cfg.dictionary( dictionary->info().title ) != nullptr )
				{
					continue;
				}
				auto       added = dict::profile( *dictionary );
				const auto at    = static_cast<std::ptrdiff_t>( dict::insertionPoint( placed, added ) );
				cfg.dictionaries.insert( cfg.dictionaries.begin() + at, { .title = dictionary->info().title, .enabled = true } );
				placed.insert( placed.begin() + at, std::move( added ) );
				changed = true;
			}
		}

		std::vector<lookup::LoadedDictionary> enabled;
		for ( const auto& preference : cfg.dictionaries )
		{
			const auto it = std::ranges::find_if( *installed, [&]( const auto& d ) { return d->info().title == preference.title; } );
			if ( it == installed->end() || !preference.enabled )
			{
				continue;
			}
			( *it )->prefetchIndexes();
			enabled.push_back(
					{
							.dictionary = *it,
							.styles     = std::make_shared<const dict::StyleSheet>( dict::StyleSheet::parse( ( *it )->styles() ) ),
							.name       = ( *it )->info().title,
					}
			);
		}

		if ( changed )
		{
			if ( auto saved = cfg.save( config_path_ ); !saved )
			{
				log::warn( "{}", saved.error().message );
			}
			config_.store( std::make_shared<const config::Config>( std::move( cfg ) ) );
		}
		auto languages = std::make_shared<std::vector<std::string>>();
		for ( const auto& loaded : enabled )
		{
			if ( auto code = loaded.dictionary->terms().empty() ? std::string() : dict::dictionaryLanguage( *loaded.dictionary ); !code.empty() && !std::ranges::contains( *languages, code ) )
			{
				languages->push_back( std::move( code ) );
			}
		}
		dictionary_languages_.store( std::move( languages ) );
		installed_.store( std::move( installed ) );
		dictionaries_.store( std::make_shared<const lookup::DictionarySet>( std::move( enabled ) ) );
		// OCR follows the languages of the dictionaries.
		capture_requests_.post( { .refresh = true } );
	}

	std::vector<const lang::Language*> Daemon::languagesInUse( const config::Config& cfg ) const
	{
		auto                               enabled = lang::enabledLanguages( cfg.disabled_languages );
		const auto                         codes   = dictionary_languages_.load();
		std::vector<const lang::Language*> used;
		if ( codes )
		{
			std::ranges::copy_if( enabled, std::back_inserter( used ), [&]( const lang::Language* language ) { return std::ranges::contains( *codes, language->code() ); } );
		}
		return used.empty() ? enabled : used;
	}

	render::PopupStyle Daemon::styleFor( const config::Config& cfg ) const
	{
		const auto& popup = cfg.popup;
		const bool  dark  = popup.theme == config::Theme::Dark || ( popup.theme == config::Theme::Auto && backend_ && backend_->prefersDarkTheme() );

		const auto colour = []( const std::string& hex ) { return hex.empty() ? std::nullopt : render::Color::parse( hex ); };

		render::PopupStyle style;
		style.theme = render::Theme::make( schemeOf( popup.scheme ), dark ).withColors( colour( popup.background_color ), colour( popup.text_color ), colour( popup.accent_color ), colour( popup.border_color ) );
		for ( const auto& named : popup.colors )
		{
			if ( const auto value = render::Color::parse( named.value ) )
			{
				( void )style.theme.set( named.name, *value );
			}
		}
		style.scale            = popup.scale > 0.0 ? popup.scale : desktopScale( backend_.get() );
		style.width            = popup.width;
		style.max_height       = popup.max_height;
		style.font_size        = popup.font_size;
		style.font_family      = popup.font_family;
		style.show_frequencies = popup.show_frequencies;
		style.show_pitch       = popup.show_pitch;
		style.show_tags        = popup.show_tags;
		style.show_furigana    = popup.show_furigana;
		style.rounded          = !backend_ || backend_->supportsTransparency();
		style.audio_button     = popup.show_buttons && cfg.audio.enabled && !cfg.audio.sources.empty();
		style.anki_button      = popup.show_buttons && cfg.anki.enabled;
		style.design           = designOf( popup.design );
		style.padding          = popup.padding;
		style.corner_radius    = popup.corner_radius;
		style.border_width     = popup.border_width;
		style.opacity          = popup.opacity / 100.0;
		style.headword_size    = popup.headword_size;
		style.furigana_size    = popup.furigana_size;
		style.show_reading     = popup.show_reading;
		style.show_inflection  = popup.show_inflection;
		style.show_dictionary  = popup.show_dictionary;
		style.show_kanji       = popup.show_kanji;
		style.max_senses       = popup.max_senses;
		return style;
	}

	lookup::LookupOptions Daemon::lookupOptions( const config::Config& cfg )
	{
		return {
			.max_length       = static_cast<std::size_t>( cfg.scan.max_length ),
			.max_results      = static_cast<std::size_t>( cfg.popup.max_results ),
			.search_kanji     = cfg.scan.search_kanji,
			.max_dictionaries = static_cast<std::size_t>( cfg.popup.max_dictionaries ),
			.language         = lang::findLanguage( cfg.language ),
			.languages        = lang::enabledLanguages( cfg.disabled_languages ),
		};
	}

	lookup::LookupResult Daemon::sampleLookup( const std::shared_ptr<const lookup::DictionarySet>& set, const config::Config& cfg )
	{
		for ( const lang::Language* language : lang::enabledLanguages( cfg.disabled_languages ) )
		{
			for ( const std::string_view word : language->sampleWords() )
			{
				if ( auto result = ipc_translator_.lookup( set, word, lookupOptions( cfg ) ); !result.empty() )
				{
					return result;
				}
			}
		}
		return {};
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------------------------------------------------

	void Daemon::onScan( platform::Point point, const platform::WindowInfo& window )
	{
		const auto cfg = config();
		if ( window.own || cfg->paused )
		{
			return;
		}
		if ( std::ranges::any_of( cfg->scan.ignored_windows, [&]( const std::string& pattern ) { return globMatch( pattern, window.wm_class ); } ) )
		{
			return;
		}
		forced_length_ = 0;
		capture_requests_.post( { .generation = ++generation_, .point = point, .window = window } );
	}

	void Daemon::onSelection( std::string text, platform::Point point )
	{
		if ( config()->paused )
		{
			return;
		}
		capture_requests_.post( { .generation = ++generation_, .point = point, .text = std::move( text ) } );
	}

	void Daemon::onClickOutside()
	{
		if ( backend_->popupVisible() )
		{
			++generation_;
			dismiss();
		}
	}

	void Daemon::dismiss()
	{
		backend_->hidePopup();
		backend_->hideHighlight();
		current_ = {};
		shown_key_.store( 0 );
		cancelled_.store( generation_.load() );
		autoplayed_.clear();
	}

	// A popup belongs to the latest scan, or shows exactly what the latest scan found (the pointer moved within the same
	// text meanwhile) and nothing was dismissed since it was asked for.
	bool Daemon::current( std::uint64_t generation, std::uint64_t key ) const
	{
		return generation == generation_.load() || ( key != 0 && key == latest_key_.load() && generation > cancelled_.load() );
	}

	// Whether what a capture found is already on screen, or on its way there.
	bool Daemon::showing( std::uint64_t key, std::uint64_t posted_key, std::uint64_t posted_generation ) const
	{
		return key != 0 && ( key == shown_key_.load() || ( key == posted_key && posted_generation > cancelled_.load() ) );
	}

	void Daemon::onAdjustLength( int delta )
	{
		if ( !current_.shown || current_.shown->result.text.empty() )
		{
			return;
		}
		const auto available = static_cast<int>( std::max( current_.shown->available, utf8::length( current_.shown->result.text ) ) );
		const int  now       = forced_length_ > 0 ? forced_length_ : static_cast<int>( current_.shown->result.matched_length );
		forced_length_       = std::clamp( now + delta, 1, std::max( 1, available ) );
		capture_requests_.post( { .generation = ++generation_, .length = forced_length_ } );
	}

	void Daemon::onPopupAction( std::size_t entry, render::PopupAction action )
	{
		log::debug( "popup action {} on entry {}", action == render::PopupAction::Audio ? "audio" : "anki", entry );
		const auto& shown = current_.shown;
		if ( !shown || entry >= shown->result.terms.size() )
		{
			return;
		}
		if ( action == render::PopupAction::Audio )
		{
			playAudio( shown, entry, true );
		}
		else if ( entry < current_.notes.size() && current_.notes[entry] == render::NoteState::Exists && !config()->anki.allow_duplicates )
		{
			backend_->showBadge( "Already in Anki", std::chrono::milliseconds( 1400 ) );
		}
		else
		{
			addToAnki( shown, entry );
		}
	}

	void Daemon::playAudio( std::shared_ptr<const Shown> shown, std::size_t entry, bool report )
	{
		( void )actions_->submit( [this, shown = std::move( shown ), entry, settings = config()->audio, report] {
			const auto& term = shown->result.terms[entry];
			if ( auto played = audio_.play( term.expression, term.reading, shown->result.language, settings ); !played && report )
			{
				backend_->post( [this, message = played.error().message] { backend_->showBadge( message, std::chrono::milliseconds( 1800 ) ); } );
			}
		} );
	}

	void Daemon::checkNotes( CurrentPopup popup )
	{
		( void )actions_->submit( [this, popup = std::move( popup ), cfg = config()]() mutable {
			// Popups change quickly while the pointer moves; only the one still on screen is checked.
			if ( popup.generation != generation_.load() )
			{
				return;
			}
			const NoteContext context{ .sentence = popup.shown->sentence, .sentence_offset = popup.shown->sentence_offset };
			auto              addable = canAddNotes( cfg->anki, popup.shown->result, context );
			if ( !addable || popup.generation != generation_.load() || std::ranges::all_of( *addable, []( bool can ) { return can; } ) )
			{
				return;
			}
			popup.notes.clear();
			for ( const bool can : *addable )
			{
				popup.notes.push_back( can ? render::NoteState::New : render::NoteState::Exists );
			}
			render_requests_.post( { .generation = popup.generation, .shown = popup.shown, .anchor = popup.anchor, .highlight = popup.highlight, .notes = std::move( popup.notes ), .source = popup.source } );
		} );
	}

	void Daemon::addToAnki( std::shared_ptr<const Shown> shown, std::size_t entry )
	{
		const auto cfg = config();
		backend_->showBadge( "Adding…", std::chrono::seconds( 20 ) );
		( void )actions_->submit( [this, shown = std::move( shown ), entry, cfg, generation = current_.generation] {
			const auto& term = shown->result.terms[entry];
			NoteContext context{ .sentence = shown->sentence, .sentence_offset = shown->sentence_offset };
			const bool  wants_audio = cfg->audio.enabled && std::ranges::any_of( cfg->anki.fields, []( const config::AnkiField& field ) { return field.value.contains( "{audio}" ); } );
			if ( wants_audio )
			{
				if ( auto clip = audio_.find( term.expression, term.reading, shown->result.language, cfg->audio ) )
				{
					context.audio = *clip;
				}
			}
			auto        added   = addNote( cfg->anki, shown->result, term, context );
			std::string message = added ? std::string( "Added to Anki ✓" ) : added.error().message;
			if ( !added )
			{
				log::warn( "anki: {}", message );
			}
			const bool present = added.has_value() || message == "Already in Anki";
			backend_->post( [this, message = std::move( message ), ok = added.has_value(), present, generation, entry] {
				backend_->showBadge( message, std::chrono::milliseconds( ok ? 1400 : 2600 ) );
				// The button turns into a check mark once Anki has the note.
				if ( present && current_.generation == generation && current_.shown )
				{
					auto notes = current_.notes;
					notes.resize( std::max( notes.size(), current_.shown->result.terms.size() ), render::NoteState::Unknown );
					notes[entry] = render::NoteState::Exists;
					render_requests_.post( { .generation = generation, .shown = current_.shown, .anchor = current_.anchor, .highlight = current_.highlight, .notes = std::move( notes ), .source = current_.source } );
				}
			} );
		} );
	}

	void Daemon::refreshCapture( CaptureState& state, const config::Config& cfg )
	{
		const auto& scan   = cfg.scan;
		std::string wanted = std::format( "{}|{}|{}|{}|{}", scan.accessibility, static_cast<int>( scan.ocr ), scan.ocr_vertical, scan.ocr_model, static_cast<int>( scan.ocr_engine ) );
		for ( const auto& pattern : scan.ocr_windows )
		{
			wanted.append( "|" ).append( pattern );
		}
		// OCR only loads the recognisers of the languages in use.
		const auto in_use = languagesInUse( cfg );
		for ( const lang::Language* language : in_use )
		{
			wanted.append( "|+" ).append( language->code() );
		}
		if ( state.capture && wanted == state.signature && state.resets == capture_resets_.load() )
		{
			return;
		}
		state.last.reset();
		state.capture.reset();
		config::Config effective = cfg;
		effective.disabled_languages.clear();
		for ( const lang::Language* language : lang::languages() )
		{
			if ( !std::ranges::contains( in_use, language ) )
			{
				effective.disabled_languages.emplace_back( language->code() );
			}
		}
		state.capture     = backend_->createTextCapture( effective );
		state.upkeep      = state.capture ? state.capture->idle() : std::nullopt;
		state.signature   = std::move( wanted );
		state.resets      = capture_resets_.load();
		std::string label = state.capture ? state.capture->describe() : std::string( "unavailable" );
		log::info( "text capture: {}", label );
		const std::scoped_lock lock( capture_status_mutex_ );
		capture_status_ = std::move( label );
	}

	Daemon::Reading Daemon::readRequest( CaptureState& state, CaptureRequest& request, const config::Config& cfg )
	{
		Reading reading;
		if ( request.length > 0 )
		{
			reading.captured = state.last;
		}
		else if ( request.text )
		{
			reading.captured = platform::CapturedText{ .text = std::move( *request.text ), .character = pointBox( request.point ) };
		}
		else if ( state.capture )
		{
			const auto started = std::chrono::steady_clock::now();
			{
				const BusyMark busy( capture_busy_since_ );
				reading.captured = state.capture->capture( request.point, request.window, static_cast<std::size_t>( cfg.scan.max_length ) );
			}
			reading.took = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started );
			log::debug( "capture: {} characters in {} ms", characters( reading.captured ), reading.took.count() );
			if ( reading.captured && reading.captured->origin != nullptr )
			{
				reading.source = reading.captured->origin->name();
			}
			// Interface text in other languages would only bring up noise.
			if ( reading.captured && cfg.scan.known_languages_only && !lang::startsInKnownScript( reading.captured->text, lang::enabledLanguages( cfg.disabled_languages ) ) )
			{
				reading.unread = std::move( reading.captured->text );
				reading.captured.reset();
			}
		}
		return reading;
	}

	void Daemon::captureLoop( const std::stop_token& stop )
	{
		thread::setName( "lg-capture" );
		CaptureState state;
		refreshCapture( state, *config() );
		lookup::Translator translator;
		std::uint64_t      last_source       = 0;
		std::uint64_t      posted_key        = 0;
		std::uint64_t      posted_generation = 0;

		CaptureReport reported;

		while ( !stop.stop_requested() )
		{
			auto request = state.upkeep ? capture_requests_.wait( stop, *state.upkeep ) : capture_requests_.wait( stop );
			if ( !request )
			{
				// Nothing asked for a while: the capture's upkeep.
				if ( state.capture && !stop.stop_requested() )
				{
					state.upkeep = state.capture->idle();
				}
				continue;
			}
			if ( request->refresh )
			{
				refreshCapture( state, *config() );
				continue;
			}
			if ( request->diagnose )
			{
				refreshCapture( state, *config() );
				std::vector<health::Check> checks;
				if ( state.capture )
				{
					const BusyMark busy( capture_busy_since_ );
					state.capture->diagnose( checks );
				}
				request->diagnose->set_value( std::move( checks ) );
				continue;
			}
			if ( request->generation != generation_.load() && !request->probe )
			{
				continue;
			}
			const auto cfg = config();
			refreshCapture( state, *cfg );

			auto [captured, source, unread, took] = readRequest( state, *request, *cfg );

			if ( request->probe )
			{
				request->probe->set_value( captured );
				continue;
			}

			if ( !request->text && request->length == 0 )
			{
				state.last  = captured;
				last_source = request->window.id;
			}
			if ( !captured )
			{
				captured = platform::CapturedText{};
			}
			auto options = lookupOptions( *cfg );
			if ( request->length > 0 )
			{
				options.max_length = static_cast<std::size_t>( request->length );
				options.selection  = true;
			}
			auto result = translator.lookup( dictionaries_.load(), captured->text, options );
			log::debug( "lookup: {} terms in {} us", result.terms.size(), std::chrono::duration_cast<std::chrono::microseconds>( result.elapsed ).count() );
			// Only real lookups count towards the statistics shown in the settings application.
			if ( !captured->text.empty() )
			{
				++lookups_;
				lookup_nanoseconds_ += static_cast<std::uint64_t>( result.elapsed.count() );
			}
			if ( !request->text && request->length == 0 )
			{
				reportCapture( *request, captured->text, unread, source, took, result.terms.size() + result.kanji.size(), reported );
			}

			if ( result.empty() )
			{
				latest_key_.store( 0 );
				if ( cfg->scan.hide_on_no_result )
				{
					backend_->post( [this, generation = request->generation] {
						if ( generation == generation_.load() )
						{
							dismiss();
						}
					} );
				}
				continue;
			}

			std::optional<platform::Rect> highlight;
			if ( cfg->scan.highlight && state.capture && !request->text )
			{
				// A length chosen with the wheel is highlighted as chosen; otherwise the match.
				highlight = state.capture->bounds( *captured, request->length > 0 ? static_cast<std::size_t>( request->length ) : result.matched_length );
			}
			const auto anchor = highlight.value_or( captured->character );
			// Moving within the same text finds the same thing again: the popup on screen, or the one on its way, stays.
			const std::uint64_t key = popupKey( !request->text.has_value(), *captured, result.matched_length, request->length, anchor );
			latest_key_.store( key );
			if ( showing( key, posted_key, posted_generation ) )
			{
				continue;
			}
			posted_key                  = key;
			posted_generation           = request->generation;
			std::string sentence        = captured->sentence.empty() ? captured->text : std::move( captured->sentence );
			const auto  sentence_offset = captured->sentence.empty() ? 0 : captured->sentence_offset;
			const auto  available       = utf8::length( captured->text );
			auto        shown           = std::make_shared<const Shown>( Shown{ .result = std::move( result ), .sentence = std::move( sentence ), .sentence_offset = sentence_offset, .available = available } );
			render_requests_.post( { .generation = request->generation, .shown = std::move( shown ), .anchor = anchor, .highlight = highlight, .source = request->length > 0 ? last_source : request->window.id, .key = key } );
		}
	}

	void Daemon::renderLoop( const std::stop_token& stop )
	{
		thread::setName( "lg-render" );
		render::PopupRenderer renderer;
		auto                  style = styleFor( *config() );
		renderer.setStyle( style );
		renderer.warmUp();

		// A first image holds what fits on screen, which is quick while the pointer moves; the whole popup follows once no
		// newer request came for a moment (the pointer rests).
		std::optional<RenderRequest> rest;
		while ( !stop.stop_requested() )
		{
			auto       request  = rest ? render_requests_.wait( stop, std::chrono::milliseconds( 250 ) ) : render_requests_.wait( stop );
			const bool complete = !request.has_value();
			if ( complete )
			{
				request = std::exchange( rest, std::nullopt );
			}
			rest.reset();
			if ( !request || !current( request->generation, request->key ) )
			{
				continue;
			}
			const auto cfg    = config();
			auto       wanted = styleFor( *cfg );
			if ( wanted != style )
			{
				style = std::move( wanted );
				renderer.setStyle( style );
			}

			const int                                 visible = complete ? 0 : style.px( style.max_height ) + style.px( 60 );
			const auto                                started = std::chrono::steady_clock::now();
			std::shared_ptr<const render::PopupImage> image;
			{
				const BusyMark busy( render_busy_since_ );
				image = renderer.render( request->shown->result, request->notes, visible );
			}
			log::debug( "render: {} terms in {} ms{}", request->shown->result.terms.size(), std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count(), complete ? " (the rest)" : "" );
			if ( !image )
			{
				continue;
			}
			const bool whole = complete || image->height() < visible;
			if ( !whole )
			{
				rest = *request;
			}
			auto shown = request->shown;

			const auto color = render::Color::parse( cfg->popup.highlight_color ).value_or( render::Color{ .r = 0.35, .g = 0.63, .b = 1.0, .a = 0.35 } );
			backend_->post( [this, generation = request->generation, image = std::move( image ), shown = std::move( shown ), notes = request->notes, style, anchor = request->anchor, highlight = request->highlight, source = request->source, key = request->key, color, autoplay = cfg->audio.autoplay, whole] {
				if ( !current( generation, key ) )
				{
					return;
				}
				if ( highlight )
				{
					backend_->showHighlight( *highlight, color );
				}
				else
				{
					backend_->hideHighlight();
				}
				backend_->showPopup( { .image = image, .style = style, .anchor = anchor, .source = source } );
				current_ = { .generation = generation, .shown = shown, .anchor = anchor, .highlight = highlight, .notes = notes, .source = source };
				shown_key_.store( key );
				// Autoplay once per word, not on every rescan while the pointer moves over it.
				if ( autoplay && style.audio_button && !shown->result.terms.empty() )
				{
					std::string word = std::format( "{}\t{}", shown->result.terms.front().expression, shown->result.terms.front().reading );
					if ( word != autoplayed_ )
					{
						autoplayed_ = std::move( word );
						playAudio( shown, 0, false );
					}
				}
				// Mark entries Anki already has; only the final image of a popup asks.
				if ( whole && style.anki_button && notes.empty() )
				{
					checkNotes( current_ );
				}
			} );
		}
	}

	void Daemon::reportCapture( const CaptureRequest& request, std::string_view text, std::string_view unread, std::string_view source, std::chrono::milliseconds took, std::size_t entries, CaptureReport& reported )
	{
		std::string summary;
		if ( !text.empty() )
		{
			summary = std::format( "«{}» read through {} in {} ms: {} {}", excerpt( text ), source, took.count(), entries, entries == 1 ? "entry" : "entries" );
		}
		else if ( !unread.empty() )
		{
			summary = std::format( "«{}» read through {} in {} ms, but it is in none of the supported languages", excerpt( unread ), source, took.count() );
		}
		else
		{
			summary = std::format( "nothing readable there ({} ms)", took.count() );
		}
		std::string where = std::format( "at {},{}", request.point.x, request.point.y );
		if ( !request.window.wm_class.empty() )
		{
			where += std::format( " in {}", request.window.wm_class );
		}

		const auto now = std::chrono::steady_clock::now();
		{
			const std::scoped_lock lock( problems_mutex_ );
			last_capture_      = std::format( "{} ({})", summary, where );
			last_capture_time_ = now;
		}
		// Shown live in the settings application; a few a second are plenty.
		if ( now - reported.at < std::chrono::milliseconds( 150 ) && summary == reported.summary )
		{
			return;
		}
		reported = { .at = now, .summary = summary };
		json::Writer out;
		out.beginObject().field( "summary", summary ).field( "where", where ).field( "found", !text.empty() ).field( "entries", entries ).endObject();
		ipc_.broadcast( "capture.result", out.str() );
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Imports
	// ---------------------------------------------------------------------------------------------------------------------

	void Daemon::importLoop( const std::stop_token& stop )
	{
		thread::setName( "lg-imports" );
		while ( true )
		{
			ImportJob job;
			{
				std::unique_lock lock( import_mutex_ );
				if ( !import_ready_.wait( lock, stop, [this] { return !imports_.empty(); } ) )
				{
					return;
				}
				job = std::move( imports_.front() );
				imports_.pop_front();
			}
			runImport( job );
		}
	}

	void Daemon::runImport( const ImportJob& job )
	{
		const auto started = [&] {
			json::Writer out;
			out.beginObject().field( "job", job.id ).field( "path", job.path.string() ).endObject();
			return out.take();
		}();
		ipc_.broadcast( "import.started", started );

		dict::ImportOptions options;
		options.background  = true;
		options.stop        = import_thread_.get_stop_token();
		auto last_report    = std::chrono::steady_clock::time_point{};
		options.on_progress = [&]( const dict::ImportProgress& progress ) {
			const auto now = std::chrono::steady_clock::now();
			if ( now - last_report < std::chrono::milliseconds( 100 ) && progress.done != progress.total )
			{
				return;
			}
			last_report = now;
			json::Writer out;
			out.beginObject().field( "job", job.id ).field( "stage", progress.stage ).field( "done", progress.done ).field( "total", progress.total ).endObject();
			ipc_.broadcast( "import.progress", out.str() );
		};

		const auto summary = store_.install( job.path, options, job.replace );
		if ( job.delete_source )
		{
			std::error_code ec;
			std::filesystem::remove( job.path, ec );
		}

		json::Writer out;
		out.beginObject().field( "job", job.id ).field( "ok", summary.has_value() );
		if ( summary )
		{
			log::info( "imported {} ({} terms, {} meta, {} kanji) in {:.2f}s", summary->title, summary->terms, summary->meta, summary->kanji, summary->seconds );
			if ( !job.replaces.empty() && job.replaces != summary->title )
			{
				( void )store_.remove( job.replaces );
			}
			out.field( "title", summary->title )
					.field( "terms", summary->terms )
					.field( "meta", summary->meta )
					.field( "kanji", summary->kanji )
					.field( "seconds", summary->seconds );
		}
		else
		{
			log::warn( "import of {} failed: {}", job.path.string(), summary.error().message );
			out.field( "error", summary.error().message );
		}
		out.endObject();

		reloadDictionaries();
		ipc_.broadcast( "import.finished", out.str() );
		ipc_.broadcast( "dictionaries.changed", "{}" );
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// IPC
	// ---------------------------------------------------------------------------------------------------------------------

	std::string Daemon::statusJson() const
	{
		const auto   cfg     = config();
		const auto   lookups = lookups_.load();
		json::Writer out;
		out.beginObject();
		out.field( "version", version );
		out.field( "protocol", protocol_version );
		out.field( "pid", static_cast<std::int64_t>( ::getpid() ) );
		out.field( "uptime", std::chrono::duration_cast<std::chrono::seconds>( std::chrono::steady_clock::now() - started_ ).count() );
		out.field( "backend", backend_ ? backend_->name() : std::string_view( "none" ) );
		{
			const std::scoped_lock lock( capture_status_mutex_ );
			out.field( "capture", capture_status_ );
		}
		out.field( "paused", cfg->paused );
		out.field( "scale", backend_ ? backend_->scaleFactor() : 1.0 );
		out.field( "dark_theme", backend_ && backend_->prefersDarkTheme() );
		out.field( "transparency", backend_ && backend_->supportsTransparency() );
		out.field( "dictionaries", dictionaries_.load()->all().size() );
		out.key( "languages" ).beginArray();
		for ( const lang::Language* language : languagesInUse( *config() ) )
		{
			out.value( language->code() );
		}
		out.endArray();
		out.field( "lookups", lookups );
		out.field( "average_lookup_us", lookups > 0 ? static_cast<double>( lookup_nanoseconds_.load() ) / static_cast<double>( lookups ) / 1000.0 : 0.0 );
		out.field( "config_path", config_path_.string() );
		out.field( "dictionaries_dir", store_.directory().string() );
		out.endObject();
		return out.take();
	}

	void Daemon::registerActionHandlers()
	{
		ipc_.on( "audio.play", [this]( const json::Value& params ) -> Result<std::string> {
			auto done = actions_->submit( [this, expression = std::string( params["expression"].asString() ), reading = std::string( params["reading"].asString() ), cfg = config()] {
				return audio_.play( expression, reading, &lang::languageOf( expression, lang::findLanguage( cfg->language ), lang::enabledLanguages( cfg->disabled_languages ) ), cfg->audio );
			} );
			if ( done.wait_for( std::chrono::seconds( 20 ) ) != std::future_status::ready )
			{
				return fail( "audio timed out" );
			}
			if ( auto played = done.get(); !played )
			{
				return std::unexpected( played.error() );
			}
			return "{}";
		} );

		ipc_.on( "anki.add", [this]( const json::Value& params ) -> Result<std::string> {
			const auto text  = params["text"].asString();
			const auto entry = static_cast<std::size_t>( std::max<std::int64_t>( 0, params["entry"].asInt( 0 ) ) );
			auto       shown = std::make_shared<Shown>();
			shown->result    = ipc_translator_.lookup( dictionaries_.load(), text, lookupOptions( *config() ) );
			shown->sentence  = std::string( params["sentence"].asString( text ) );
			if ( entry >= shown->result.terms.size() )
			{
				return fail( "no such entry" );
			}
			auto done = actions_->submit( [this, shown, entry, cfg = config()]() -> Result<std::int64_t> {
				const auto& term = shown->result.terms[entry];
				NoteContext context{ .sentence = shown->sentence, .sentence_offset = shown->sentence.find( textOf( shown->result ) ) };
				if ( auto clip = audio_.find( term.expression, term.reading, shown->result.language, cfg->audio ); clip && cfg->audio.enabled )
				{
					context.audio = *clip;
				}
				return addNote( cfg->anki, shown->result, term, context );
			} );
			if ( done.wait_for( std::chrono::seconds( 30 ) ) != std::future_status::ready )
			{
				return fail( "Anki timed out" );
			}
			auto added = done.get();
			if ( !added )
			{
				return std::unexpected( added.error() );
			}
			return std::format( "{{\"note\":{}}}", *added );
		} );
	}

	void Daemon::registerHandlers()
	{
		registerActionHandlers();
		ipc_.on( "status", [this]( const json::Value& ) -> Result<std::string> { return statusJson(); } );

		ipc_.on( "config.get", [this]( const json::Value& ) -> Result<std::string> { return "{\"config\":" + config()->toJson( false ) + "}"; } );

		ipc_.on( "config.set", [this]( const json::Value& params ) -> Result<std::string> {
			auto parsed = config::Config::fromJson( params["config"] );
			if ( !parsed )
			{
				return std::unexpected( parsed.error() );
			}
			if ( auto applied = applyConfig( std::move( *parsed ), true ); !applied )
			{
				return std::unexpected( applied.error() );
			}
			return "{}";
		} );

		ipc_.on( "scan.pause", [this]( const json::Value& params ) -> Result<std::string> {
			auto cfg   = *config();
			cfg.paused = params["paused"].isBool() ? params["paused"].asBool() : !cfg.paused;
			if ( auto applied = applyConfig( std::move( cfg ), true ); !applied )
			{
				return std::unexpected( applied.error() );
			}
			ipc_.broadcast( "status.changed", statusJson() );
			return std::format( "{{\"paused\":{}}}", config()->paused );
		} );

		ipc_.on( "dictionaries.list", [this]( const json::Value& ) -> Result<std::string> {
			const auto   cfg       = config();
			const auto   installed = installed_.load();
			json::Writer out;
			out.beginObject().key( "dictionaries" ).beginArray();
			for ( const auto& preference : cfg->dictionaries )
			{
				const auto it = std::ranges::find_if( *installed, [&]( const auto& d ) { return d->info().title == preference.title; } );
				if ( it != installed->end() )
				{
					ipc::writeDictionary( out, **it, preference.enabled );
				}
			}
			out.endArray().endObject();
			return out.take();
		} );

		ipc_.on( "dictionaries.import", [this]( const json::Value& params ) -> Result<std::string> {
			const auto path = std::string( params["path"].asString() );
			if ( path.empty() )
			{
				return fail( "missing \"path\"" );
			}
			const auto id = next_job_++;
			{
				const std::scoped_lock lock( import_mutex_ );
				imports_.push_back(
						{
								.id            = id,
								.path          = path,
								.replace       = params["replace"].asBool(),
								.delete_source = params["delete_source"].asBool(),
								.replaces      = std::string( params["replaces"].asString() ),
						}
				);
			}
			import_ready_.notify_one();
			return std::format( "{{\"job\":{}}}", id );
		} );

		ipc_.on( "dictionaries.remove", [this]( const json::Value& params ) -> Result<std::string> {
			if ( auto removed = store_.remove( params["title"].asString() ); !removed )
			{
				return std::unexpected( removed.error() );
			}
			reloadDictionaries();
			ipc_.broadcast( "dictionaries.changed", "{}" );
			return "{}";
		} );

		// The priority list in its smart order: word dictionaries (Jitendex, JMdict first), names, grammar, specialised,
		// monolingual, kanji, frequency and pitch lists. Whether each is enabled stays as it was.
		// `dry_run` only tells the order.
		ipc_.on( "dictionaries.sort", [this]( const json::Value& params ) -> Result<std::string> {
			auto                                      cfg      = *config();
			const auto                                profiles = profilesOf( cfg.dictionaries, *installed_.load() );
			std::vector<config::DictionaryPreference> sorted;
			sorted.reserve( cfg.dictionaries.size() );
			json::Writer out;
			out.beginObject().key( "order" ).beginArray();
			for ( const std::size_t index : dict::smartOrder( profiles ) )
			{
				sorted.push_back( cfg.dictionaries[index] );
				out.beginObject().field( "title", cfg.dictionaries[index].title ).field( "kind", dict::kindName( dict::classify( profiles[index] ) ) ).endObject();
			}
			out.endArray().endObject();
			if ( params["dry_run"].asBool() )
			{
				return out.take();
			}
			cfg.dictionaries = std::move( sorted );
			if ( auto applied = applyConfig( std::move( cfg ), true ); !applied )
			{
				return std::unexpected( applied.error() );
			}
			ipc_.broadcast( "dictionaries.changed", "{}" );
			return out.take();
		} );

		ipc_.on( "dictionaries.reload", [this]( const json::Value& ) -> Result<std::string> {
			reloadDictionaries();
			ipc_.broadcast( "dictionaries.changed", "{}" );
			return "{}";
		} );

		registerLookupHandlers();
	}

	void Daemon::registerLookupHandlers()
	{
		ipc_.on( "lookup", [this]( const json::Value& params ) -> Result<std::string> {
			const auto format   = params["markup"].asString( "html" );
			const auto markup   = markupFor( format );
			auto       options  = lookupOptions( *config() );
			options.max_length  = static_cast<std::size_t>( std::max<std::int64_t>( 1, params["max_length"].asInt( static_cast<std::int64_t>( options.max_length ) ) ) );
			const auto   result = ipc_translator_.lookup( dictionaries_.load(), params["text"].asString(), options );
			json::Writer out;
			ipc::writeLookup( out, result, markup );
			return out.take();
		} );

		ipc_.on( "popup.show", [this]( const json::Value& params ) -> Result<std::string> {
			if ( !backend_ )
			{
				return fail( "no desktop integration available" );
			}
			// Without a text, a word the dictionaries have.
			const std::string_view text   = params["text"].asString();
			auto                   result = text.empty() ? sampleLookup( dictionaries_.load(), *config() ) : ipc_translator_.lookup( dictionaries_.load(), text, lookupOptions( *config() ) );
			if ( result.empty() )
			{
				return fail( "no results" );
			}
			std::promise<platform::Point> where;
			auto                          future = where.get_future();
			backend_->post( [this, &where] { where.set_value( backend_->pointer() ); } );
			if ( future.wait_for( std::chrono::seconds( 2 ) ) != std::future_status::ready )
			{
				return fail( "desktop is not responding" );
			}
			std::string sentence = text.empty() ? result.text : std::string( text );
			auto        shown    = std::make_shared<const Shown>( Shown{ .result = std::move( result ), .sentence = std::move( sentence ) } );
			render_requests_.post( { .generation = ++generation_, .shown = std::move( shown ), .anchor = pointBox( future.get() ) } );
			return "{}";
		} );

		ipc_.on( "popup.hide", [this]( const json::Value& ) -> Result<std::string> {
			if ( backend_ )
			{
				++generation_;
				backend_->post( [this] { dismiss(); } );
			}
			return "{}";
		} );

		ipc_.on( "popup.preview", [this]( const json::Value& params ) -> Result<std::string> {
			config::Config cfg = *config();
			if ( params["config"].isObject() )
			{
				auto parsed = config::Config::fromJson( params["config"] );
				if ( !parsed )
				{
					return std::unexpected( parsed.error() );
				}
				cfg = std::move( *parsed );
			}
			// Without a text (or with one no dictionary has) the preview shows a word the dictionaries have.
			const auto set    = dictionaries_.load();
			auto       result = ipc_translator_.lookup( set, params["text"].asString(), lookupOptions( cfg ) );
			if ( result.empty() )
			{
				result = sampleLookup( set, cfg );
			}
			if ( result.empty() )
			{
				return fail( "no results for the preview text" );
			}

			if ( !preview_renderer_ )
			{
				preview_renderer_ = std::make_unique<render::PopupRenderer>();
			}
			auto style    = styleFor( cfg );
			style.rounded = true;
			preview_renderer_->setStyle( style );
			const auto image = preview_renderer_->render( result );
			if ( !image )
			{
				return fail( "nothing to render" );
			}

			const auto size    = render::popupSize( *image, style );
			auto*      surface = cairo_image_surface_create( CAIRO_FORMAT_ARGB32, size.width, size.height );
			cairo_t*   cr      = cairo_create( surface );
			render::composePopup( cr, *image, 0, size, style );
			cairo_destroy( cr );
			std::string png;
			cairo_surface_write_to_png_stream( surface, &appendPng, &png );
			cairo_surface_destroy( surface );

			json::Writer out;
			out.beginObject().field( "width", size.width ).field( "height", size.height ).field( "scale", style.scale ).field( "png", base64( png ) ).endObject();
			return out.take();
		} );

		ipc_.on( "highlight.preview", [this]( const json::Value& params ) -> Result<std::string> {
			config::Config cfg = *config();
			if ( params["config"].isObject() )
			{
				auto parsed = config::Config::fromJson( params["config"] );
				if ( !parsed )
				{
					return std::unexpected( parsed.error() );
				}
				cfg = std::move( *parsed );
			}
			const auto scale  = styleFor( cfg ).scale;
			const auto colour = render::Color::parse( cfg.popup.highlight_color ).value_or( render::Color{ .r = 0.35, .g = 0.63, .b = 1.0, .a = 0.35 } );
			// As wide as the popup, so the settings show the two previews one above the other, lined up.
			const int    width   = static_cast<int>( std::lround( cfg.popup.width * scale ) );
			auto*        surface = render::highlightPreview( platform::lookFor( cfg.popup ), colour, cfg.popup.highlight_auto, scale, width );
			json::Writer out;
			out.beginObject()
					.field( "width", cairo_image_surface_get_width( surface ) )
					.field( "height", cairo_image_surface_get_height( surface ) )
					.field( "scale", scale )
					.field( "png", pngBase64( surface ) )
					.endObject();
			cairo_surface_destroy( surface );
			return out.take();
		} );

		ipc_.on( "debug.capture", [this]( const json::Value& params ) -> Result<std::string> {
			if ( !backend_ )
			{
				return fail( "no desktop integration available" );
			}
			auto                  probe  = std::make_shared<std::promise<std::optional<platform::CapturedText>>>();
			auto                  future = probe->get_future();
			const platform::Point point{ .x = static_cast<int>( params["x"].asInt() ), .y = static_cast<int>( params["y"].asInt() ) };
			// Resolved like a real scan, so the capture clips to the window under the point.
			backend_->post( [this, point, probe] { capture_requests_.post( { .point = point, .window = backend_->windowAt( point ), .probe = probe } ); } );
			if ( future.wait_for( std::chrono::seconds( 5 ) ) != std::future_status::ready )
			{
				return fail( "capture timed out" );
			}
			json::Writer out;
			const auto   captured = future.get();
			out.beginObject().field( "text", captured ? captured->text : std::string() );
			if ( captured )
			{
				out.field( "confidence", static_cast<double>( captured->confidence ) ).field( "source", captured->origin != nullptr ? std::string( captured->origin->name() ) : std::string() );
			}
			out.endObject();
			return out.take();
		} );

		ipc_.on( "keys.record", [this]( const json::Value& ) -> Result<std::string> {
			if ( !backend_ )
			{
				return fail( "no desktop integration available" );
			}
			backend_->post( [this] {
				backend_->recordChord( [this]( const std::vector<std::string>& keys ) {
					json::Writer out;
					out.beginObject().key( "keys" ).beginArray();
					for ( const auto& key : keys )
					{
						out.value( key );
					}
					out.endArray().endObject();
					ipc_.broadcast( "keys.recorded", out.str() );
				} );
			} );
			return "{}";
		} );

		ipc_.on( "keys.cancel", [this]( const json::Value& ) -> Result<std::string> {
			if ( backend_ )
			{
				backend_->post( [this] { backend_->cancelRecording(); } );
			}
			return "{}";
		} );

		ipc_.on( "health", [this]( const json::Value& params ) -> Result<std::string> { return healthJson( params["interactive"].asBool() ); } );

		ipc_.on( "debug.scan", [this]( const json::Value& params ) -> Result<std::string> {
			if ( !backend_ )
			{
				return fail( "no desktop integration available" );
			}
			// Exactly what holding the trigger there does: capture, look up, highlight and show the popup.
			const platform::Point point{ .x = static_cast<int>( params["x"].asInt() ), .y = static_cast<int>( params["y"].asInt() ) };
			backend_->post( [this, point] { onScan( point, backend_->windowAt( point ) ); } );
			return "{}";
		} );

		ipc_.on( "capture.reset", [this]( const json::Value& ) -> Result<std::string> {
			++capture_resets_;
			capture_requests_.post( { .refresh = true } );
			return "{}";
		} );

		ipc_.on( "shutdown", [this]( const json::Value& ) -> Result<std::string> {
			requestQuit();
			return "{}";
		} );
	}

} // namespace lexiglance::daemon
