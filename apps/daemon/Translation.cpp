#include "Translation.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/ocr/Onnx.h>
#include <lexiglance/translate/Model.h>

#include <algorithm>
#include <format>
#include <future>

namespace lexiglance::daemon
{

	namespace
	{

		// Models unused this long are let go.
		constexpr auto idle_unload = std::chrono::minutes( 10 );
		// Held at once: the language being read and the one beside it. Each is several hundred megabytes (up to a
		// gigabyte in full precision), so a third one makes the least recently used go.
		constexpr std::size_t most_models = 2;
		// Translations kept: this many at most, each for this long, and longer the more often it is asked for again (a
		// sentence read over and over is translated once), but never beyond the longest.
		constexpr std::size_t cache_size    = 256;
		constexpr auto        cache_time    = std::chrono::minutes( 10 );
		constexpr auto        cache_longest = std::chrono::hours( 2 );

		std::chrono::steady_clock::duration keptFor( std::uint32_t uses )
		{
			return std::min<std::chrono::steady_clock::duration>( cache_longest, cache_time * uses );
		}

		std::string cacheKey( const lang::Language& language, std::string_view text )
		{
			return std::format( "{}\n{}", language.code(), text );
		}

		std::filesystem::path modelDirectory( const lang::Language& language )
		{
			return paths::translationDir() / language.translationModel().directory();
		}

		// Threads for one translation: enough to be quick, few enough to leave a game its cores.
		int threads()
		{
			return static_cast<int>( std::clamp( std::thread::hardware_concurrency() / 4U, 1U, 4U ) );
		}

	} // namespace

	TranslationService::TranslationService( Done done ) :
		done_( std::move( done ) ),
		thread_( [this]( const std::stop_token& stop ) { run( stop ); } )
	{
	}

	TranslationService::~TranslationService()
	{
		thread_.request_stop();
		wake_.notify_all();
	}

	std::optional<std::string> TranslationService::cached( const lang::Language& language, std::string_view text )
	{
		const std::string      key = cacheKey( language, text );
		const std::scoped_lock lock( mutex_ );
		const auto             now = std::chrono::steady_clock::now();
		const auto             it  = std::ranges::find( cache_, key, &Remembered::key );
		if ( it == cache_.end() )
		{
			return std::nullopt;
		}
		if ( it->until <= now )
		{
			cache_.erase( it );
			return std::nullopt;
		}
		// Asked for again: kept longer.
		it->until = now + keptFor( ++it->uses );
		cache_.splice( cache_.begin(), cache_, it );
		return cache_.front().text;
	}

	void TranslationService::request( std::uint64_t ticket, const lang::Language& language, std::string text )
	{
		{
			const std::scoped_lock lock( mutex_ );
			pending_ = Request{ .ticket = ticket, .language = &language, .text = std::move( text ) };
		}
		wake_.notify_all();
	}

	Result<std::string> TranslationService::translateNow( const lang::Language& language, std::string text, std::chrono::seconds timeout )
	{
		// Shared: this may stop waiting before the translation thread is done.
		auto promise = std::make_shared<std::promise<Result<std::string>>>();
		auto answer  = promise->get_future();
		{
			const std::scoped_lock lock( mutex_ );
			jobs_.emplace_back( [this, &language, text = std::move( text ), promise] {
				last_used_  = std::chrono::steady_clock::now();
				auto loaded = model( language );
				if ( !loaded )
				{
					promise->set_value( std::unexpected( loaded.error() ) );
					return;
				}
				auto translated = ( *loaded )->translate( text );
				if ( translated )
				{
					remember( language, text, *translated );
				}
				promise->set_value( std::move( translated ) );
			} );
		}
		wake_.notify_all();
		if ( answer.wait_for( timeout ) != std::future_status::ready )
		{
			return fail( "the translation took longer than {} seconds", timeout.count() );
		}
		return answer.get();
	}

	std::string TranslationService::unavailable( const lang::Language& language )
	{
		const auto& model = language.translationModel();
		if ( model.empty() )
		{
			return std::format( "{} has no translation model.", language.name() );
		}
		if ( !translate::downloaded( modelDirectory( language ), translate::Precision::Compact ) )
		{
			return std::format( "Translating {} needs its model: Translation page, Download.", language.name() );
		}
		return {};
	}

	void TranslationService::setPrecisions( std::map<std::string, translate::Precision> languages, translate::Precision others )
	{
		{
			const std::scoped_lock lock( mutex_ );
			if ( precisions_ == languages && others_ == others )
			{
				return;
			}
			precisions_ = std::move( languages );
			others_     = others;
			cache_.clear();
			reload_ = true;
		}
		wake_.notify_all();
	}

	translate::Precision TranslationService::precisionFor( const lang::Language& language ) const
	{
		const std::scoped_lock lock( mutex_ );
		const auto             it = precisions_.find( std::string( language.code() ) );
		return it != precisions_.end() ? it->second : others_;
	}

	void TranslationService::warm( const lang::Language& language )
	{
		if ( !unavailable( language ).empty() )
		{
			return;
		}
		{
			const std::scoped_lock lock( mutex_ );
			if ( std::exchange( warming_, true ) )
			{
				return;
			}
			jobs_.emplace_back( [this, &language] {
				{
					const std::scoped_lock done( mutex_ );
					warming_ = false;
				}
				last_used_ = std::chrono::steady_clock::now();
				if ( auto loaded = model( language ); !loaded )
				{
					log::debug( "translation: {}", loaded.error().message );
				}
			} );
		}
		wake_.notify_all();
	}

	void TranslationService::reload()
	{
		{
			const std::scoped_lock lock( mutex_ );
			reload_ = true;
		}
		wake_.notify_all();
	}

	Result<translate::Model*> TranslationService::model( const lang::Language& language )
	{
		const std::string directory = language.translationModel().directory();
		const auto        wanted    = precisionFor( language );
		// The weights asked for, or the others while only they are downloaded.
		const auto precision = translate::downloaded( modelDirectory( language ), wanted ).value_or( wanted );
		const auto it        = models_.find( directory );
		if ( it != models_.end() && it->second.precision == precision )
		{
			it->second.used = std::chrono::steady_clock::now();
			return it->second.model.get();
		}
		if ( std::string missing = unavailable( language ); !missing.empty() )
		{
			return fail( "{}", missing );
		}
		// Another precision was asked for since: that model goes, so both are never held at once.
		if ( it != models_.end() )
		{
			log::debug( "translation: {} was loaded in {}, {} was asked for", directory, translate::precisionName( it->second.precision ), translate::precisionName( precision ) );
			models_.erase( it );
		}
		const auto started = std::chrono::steady_clock::now();
		auto       loaded  = translate::Model::load( modelDirectory( language ), precision, paths::ocrDir() / "runtime", threads() );
		if ( !loaded )
		{
			return fail( "the {} translation model does not load: {}", language.name(), loaded.error().message );
		}
		// Room for it: the model that has gone longest without translating goes (the health check tries every one).
		while ( models_.size() >= most_models )
		{
			const auto oldest = std::ranges::min_element( models_, {}, []( const auto& held ) { return held.second.used; } );
			log::debug( "translation: {} let go of, {} models are kept", oldest->first, most_models );
			models_.erase( oldest );
		}
		log::info( "translation: {} ({}) loaded in {} ms", directory, translate::precisionName( precision ), std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count() );
		return models_.emplace( directory, Loaded{ .precision = precision, .used = std::chrono::steady_clock::now(), .model = std::move( *loaded ) } ).first->second.model.get();
	}

	void TranslationService::remember( const lang::Language& language, const std::string& text, const std::string& translation )
	{
		const std::scoped_lock lock( mutex_ );
		const auto             now = std::chrono::steady_clock::now();
		std::erase_if( cache_, [&]( const Remembered& kept ) { return kept.until <= now; } );
		cache_.push_front( { .key = cacheKey( language, text ), .text = translation, .uses = 1, .until = now + keptFor( 1 ) } );
		if ( cache_.size() > cache_size )
		{
			cache_.pop_back();
		}
	}

	void TranslationService::run( const std::stop_token& stop )
	{
		thread::setName( "lg-translate" );
		while ( !stop.stop_requested() )
		{
			std::optional<Request>          request;
			std::move_only_function<void()> job;
			bool                            unload = false;
			{
				std::unique_lock lock( mutex_ );
				const bool       woken = wake_.wait_for( lock, stop, idle_unload, [this] { return pending_.has_value() || !jobs_.empty() || reload_; } );
				if ( stop.stop_requested() )
				{
					return;
				}
				unload  = reload_ || ( !woken && !models_.empty() && std::chrono::steady_clock::now() - last_used_ >= idle_unload );
				reload_ = false;
				if ( !jobs_.empty() )
				{
					job = std::move( jobs_.front() );
					jobs_.pop_front();
				}
				else
				{
					request = std::exchange( pending_, std::nullopt );
				}
			}
			if ( unload && !models_.empty() )
			{
				log::debug( "translation: models unloaded" );
				models_.clear();
			}
			if ( job )
			{
				job();
				continue;
			}
			if ( !request )
			{
				continue;
			}
			last_used_  = std::chrono::steady_clock::now();
			auto loaded = model( *request->language );
			if ( !loaded )
			{
				done_( request->ticket, std::unexpected( loaded.error() ), std::chrono::microseconds::zero() );
				continue;
			}
			const auto started    = std::chrono::steady_clock::now();
			auto       translated = ( *loaded )->translate( request->text );
			const auto took       = std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - started );
			log::debug( "translation: {} characters in {} ms", request->text.size(), took.count() / 1000 );
			if ( translated )
			{
				remember( *request->language, request->text, *translated );
			}
			else
			{
				log::warn( "translation failed: {}", translated.error().message );
			}
			done_( request->ticket, std::move( translated ), took );
		}
	}

	void TranslationService::diagnose( std::span<const lang::Language* const> languages, bool enabled, std::vector<health::Check>& out )
	{
		health::Check check{ .id = "translation", .title = "Translating sentences" };
		if ( !enabled )
		{
			check.detail = "Translation is turned off (Translation page).";
			out.push_back( std::move( check ) );
			return;
		}
		std::vector<const lang::Language*> with_model;
		std::ranges::copy_if( languages, std::back_inserter( with_model ), []( const lang::Language* language ) { return !language->translationModel().empty(); } );
		if ( with_model.empty() )
		{
			check.detail = "None of the languages in use has a translation model.";
			out.push_back( std::move( check ) );
			return;
		}

		// On the translation thread, with its models: loading them twice would take twice the memory. Shared, since this
		// may stop waiting before the thread is done.
		struct Outcome
		{
			bool        downloaded = false;
			bool        works      = false;
			std::string text;
		};
		auto promise = std::make_shared<std::promise<std::vector<Outcome>>>();
		auto answer  = promise->get_future();
		{
			const std::scoped_lock lock( mutex_ );
			jobs_.emplace_back( [this, with_model, promise] {
				std::vector<Outcome> outcomes;
				for ( const lang::Language* language : with_model )
				{
					if ( !unavailable( *language ).empty() )
					{
						outcomes.push_back( {} );
						continue;
					}
					const auto started    = std::chrono::steady_clock::now();
					auto       loaded     = model( *language );
					auto       translated = loaded ? ( *loaded )->translate( language->exampleSentence() ) : Result<std::string>( std::unexpected( loaded.error() ) );
					const auto elapsed    = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
					if ( translated && !translated->empty() )
					{
						outcomes.push_back( { .downloaded = true, .works = true, .text = std::format( "{} «{}» in {} ms", language->name(), *translated, elapsed ) } );
					}
					else
					{
						outcomes.push_back( { .downloaded = true, .works = false, .text = std::format( "{} ({})", language->name(), translated ? std::string( "its test sentence came out empty" ) : translated.error().message ) } );
					}
				}
				last_used_ = std::chrono::steady_clock::now();
				promise->set_value( std::move( outcomes ) );
			} );
		}
		wake_.notify_all();
		if ( answer.wait_for( std::chrono::seconds( 90 ) ) != std::future_status::ready )
		{
			check.status = health::Severity::Warning;
			check.detail = "The translation models did not answer within 90 seconds.";
			out.push_back( std::move( check ) );
			return;
		}
		const auto outcomes = answer.get();

		std::vector<std::string> good;
		std::vector<std::string> bad;
		std::vector<std::string> missing;
		for ( std::size_t i = 0; i < with_model.size() && i < outcomes.size(); ++i )
		{
			if ( !outcomes[i].downloaded )
			{
				missing.emplace_back( with_model[i]->name() );
			}
			else
			{
				( outcomes[i].works ? good : bad ).push_back( outcomes[i].text );
			}
		}
		const auto join = []( const std::vector<std::string>& items, std::string_view separator ) {
			std::string text;
			for ( const std::string& item : items )
			{
				text.append( text.empty() ? "" : separator ).append( item );
			}
			return text;
		};
		if ( !bad.empty() )
		{
			// Windows without Microsoft's runtime, which ONNX Runtime needs: installing that is the fix, not the model.
			const bool runtime = std::ranges::any_of( bad, []( const std::string& reason ) { return reason.contains( ocr::vc_runtime_absent ); } );
			check.status       = health::Severity::Error;
			check.detail       = runtime ? std::format( "Translation does not work: {}.", join( bad, "; " ) )
			                             : std::format( "Translation does not work for {}. Download the model again (Translation page).", join( bad, "; " ) );
			check.fix          = runtime ? "install-vcredist" : "download-translation";
		}
		else if ( !missing.empty() )
		{
			check.status = health::Severity::Warning;
			check.detail = missing.size() == 1 ? std::format( "No translation model is downloaded for {}, so its sentences are not translated.", missing.front() )
			                                   : std::format( "No translation models are downloaded for {}, so their sentences are not translated.", join( missing, ", " ) );
			if ( !good.empty() )
			{
				check.detail += std::format( " Translated test sentences: {}.", join( good, "; " ) );
			}
			check.fix = "download-translation";
		}
		else
		{
			check.detail = std::format( "Translated test sentences: {}.", join( good, "; " ) );
		}
		out.push_back( std::move( check ) );
	}

} // namespace lexiglance::daemon
