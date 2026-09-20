#include "Stats.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace lexiglance::daemon
{

	namespace
	{

		// How much of the tally is kept: enough for a page of favourites and a year of days, little enough that the
		// file stays small and writing it is never felt.
		constexpr std::size_t kept_words = 2000;
		constexpr std::size_t kept_days  = 400;
		// Words are counted as they are read, so a stray line of text cannot fill the file with one enormous entry.
		constexpr std::size_t longest_word = 64;

		void readCounts( const json::Value& object, std::map<std::string, std::uint64_t, std::less<>>& counts )
		{
			for ( const json::Member& member : object.members() )
			{
				if ( member.value.isNumber() )
				{
					counts.emplace( std::string( member.key ), static_cast<std::uint64_t>( member.value.asInt() ) );
				}
			}
		}

		void writeCounts( json::Writer& out, std::string_view name, const std::map<std::string, std::uint64_t, std::less<>>& counts )
		{
			out.key( name ).beginObject();
			for ( const auto& [key, count] : counts )
			{
				out.field( key, count );
			}
			out.endObject();
		}

		// Every figure of a day, by the name it has in the file and in the Statistics page's request.
		template <typename Day>
		constexpr auto day_fields = std::to_array<std::pair<std::string_view, std::uint64_t Day::*>>( {
				{ "lookups", &Day::lookups },
				{ "found", &Day::found },
				{ "characters", &Day::characters },
				{ "popups", &Day::popups },
				{ "audio", &Day::audio },
				{ "anki", &Day::anki },
				{ "translations", &Day::translations },
				{ "translated_characters", &Day::translated_characters },
		} );

		// The most counted entries first, at most `limit` of them.
		std::vector<std::pair<std::string_view, std::uint64_t>> top( const std::map<std::string, std::uint64_t, std::less<>>& counts, std::size_t limit )
		{
			std::vector<std::pair<std::string_view, std::uint64_t>> sorted;
			sorted.reserve( counts.size() );
			for ( const auto& [key, count] : counts )
			{
				sorted.emplace_back( key, count );
			}
			std::ranges::sort( sorted, []( const auto& a, const auto& b ) { return a.second != b.second ? a.second > b.second : a.first < b.first; } );
			if ( sorted.size() > limit )
			{
				sorted.resize( limit );
			}
			return sorted;
		}

		// Keeps a map within bounds by dropping its smallest counts.
		void prune( std::map<std::string, std::uint64_t, std::less<>>& counts, std::size_t keep )
		{
			if ( counts.size() <= keep )
			{
				return;
			}
			std::map<std::string, std::uint64_t, std::less<>> smaller;
			for ( const auto& [word, count] : top( counts, keep ) )
			{
				smaller.emplace( std::string( word ), count );
			}
			counts = std::move( smaller );
		}

	} // namespace

	void Statistics::load( std::filesystem::path file )
	{
		const std::scoped_lock lock( mutex_ );
		file_  = std::move( file );
		tally_ = {};

		std::ifstream in( file_, std::ios::binary );
		std::string   text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
		if ( !text.empty() )
		{
			if ( auto document = json::Document::parse( std::move( text ) ) )
			{
				const json::Value& root         = document->root();
				tally_.lookups                  = static_cast<std::uint64_t>( root["lookups"].asInt() );
				tally_.found                    = static_cast<std::uint64_t>( root["found"].asInt() );
				tally_.characters               = static_cast<std::uint64_t>( root["characters"].asInt() );
				tally_.popups                   = static_cast<std::uint64_t>( root["popups"].asInt() );
				tally_.audio                    = static_cast<std::uint64_t>( root["audio"].asInt() );
				tally_.anki                     = static_cast<std::uint64_t>( root["anki"].asInt() );
				tally_.microseconds             = static_cast<std::uint64_t>( root["microseconds"].asInt() );
				tally_.sessions                 = static_cast<std::uint64_t>( root["sessions"].asInt() );
				tally_.first_day                = std::string( root["first_day"].asString() );
				tally_.translations             = static_cast<std::uint64_t>( root["translations"].asInt() );
				tally_.translated_characters    = static_cast<std::uint64_t>( root["translated_characters"].asInt() );
				tally_.translations_made        = static_cast<std::uint64_t>( root["translations_made"].asInt() );
				tally_.translation_microseconds = static_cast<std::uint64_t>( root["translation_microseconds"].asInt() );
				readCounts( root["languages"], tally_.languages );
				readCounts( root["translated_languages"], tally_.translated_languages );
				readCounts( root["sources"], tally_.sources );
				readCounts( root["words"], tally_.words );
				// Each day's figures; a tally from before 1.3.0 has only its lookups, as a count.
				for ( const json::Member& member : root["days"].members() )
				{
					Day& day = tally_.days[std::string( member.key )];
					if ( member.value.isNumber() )
					{
						day.lookups = static_cast<std::uint64_t>( member.value.asInt() );
						continue;
					}
					for ( const auto& [name, field] : day_fields<Day> )
					{
						day.*field = static_cast<std::uint64_t>( member.value[name].asInt() );
					}
				}
			}
			else
			{
				log::warn( "statistics: {} could not be read; counting starts again", file_.string() );
			}
		}

		const auto today = log::day( std::chrono::system_clock::now() );
		if ( tally_.first_day.empty() )
		{
			tally_.first_day = today;
		}
		++tally_.sessions;
		dirty_ = true;
	}

	void Statistics::save()
	{
		std::string           text;
		std::filesystem::path file;
		{
			const std::scoped_lock lock( mutex_ );
			if ( !dirty_ || file_.empty() )
			{
				return;
			}
			dirty_ = false;
			file   = file_;

			json::Writer out;
			out.beginObject();
			out.field( "lookups", tally_.lookups );
			out.field( "found", tally_.found );
			out.field( "characters", tally_.characters );
			out.field( "popups", tally_.popups );
			out.field( "audio", tally_.audio );
			out.field( "anki", tally_.anki );
			out.field( "microseconds", tally_.microseconds );
			out.field( "sessions", tally_.sessions );
			out.field( "first_day", tally_.first_day );
			out.field( "translations", tally_.translations );
			out.field( "translated_characters", tally_.translated_characters );
			out.field( "translations_made", tally_.translations_made );
			out.field( "translation_microseconds", tally_.translation_microseconds );
			writeCounts( out, "languages", tally_.languages );
			writeCounts( out, "translated_languages", tally_.translated_languages );
			writeCounts( out, "sources", tally_.sources );
			writeCounts( out, "words", tally_.words );
			out.key( "days" ).beginObject();
			for ( const auto& [name, day] : tally_.days )
			{
				out.key( name ).beginObject();
				for ( const auto& [field_name, field] : day_fields<Day> )
				{
					out.field( field_name, day.*field );
				}
				out.endObject();
			}
			out.endObject();
			out.endObject();
			text = out.take();
		}

		std::error_code ec;
		std::filesystem::create_directories( file.parent_path(), ec );
		// Written beside the file and moved over it, so an interrupted write never leaves half a tally behind.
		auto temporary = file;
		temporary += ".new";
		{
			std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
			out << text;
			if ( !out )
			{
				log::warn( "statistics: cannot write {}", temporary.string() );
				return;
			}
		}
		std::filesystem::rename( temporary, file, ec );
		if ( ec )
		{
			log::warn( "statistics: cannot replace {} ({})", file.string(), ec.message() );
		}
	}

	void Statistics::recordLookup( std::string_view word, std::string_view language, std::string_view source, std::size_t characters, std::chrono::microseconds took )
	{
		const std::scoped_lock lock( mutex_ );
		if ( !enabled_ )
		{
			return;
		}
		Day& day = today();
		++tally_.lookups;
		++day.lookups;
		tally_.characters += characters;
		day.characters += characters;
		tally_.microseconds += static_cast<std::uint64_t>( std::max<std::int64_t>( 0, took.count() ) );
		if ( !source.empty() )
		{
			++tally_.sources[std::string( source )];
		}
		if ( !word.empty() )
		{
			++tally_.found;
			++day.found;
			if ( !language.empty() )
			{
				++tally_.languages[std::string( language )];
			}
			if ( word.size() <= longest_word )
			{
				++tally_.words[std::string( word )];
			}
		}
		prune( tally_.words, kept_words );
		dirty_ = true;
	}

	void Statistics::recordPopup()
	{
		const std::scoped_lock lock( mutex_ );
		mark( tally_.popups, &Day::popups );
	}

	void Statistics::recordAudio()
	{
		const std::scoped_lock lock( mutex_ );
		mark( tally_.audio, &Day::audio );
	}

	void Statistics::recordAnki()
	{
		const std::scoped_lock lock( mutex_ );
		mark( tally_.anki, &Day::anki );
	}

	void Statistics::recordTranslation( std::string_view language, std::size_t characters, std::chrono::microseconds took )
	{
		const std::scoped_lock lock( mutex_ );
		if ( !translations_enabled_ )
		{
			return;
		}
		Day& day = today();
		++tally_.translations;
		++day.translations;
		tally_.translated_characters += characters;
		day.translated_characters += characters;
		if ( took.count() > 0 )
		{
			++tally_.translations_made;
			tally_.translation_microseconds += static_cast<std::uint64_t>( took.count() );
		}
		if ( !language.empty() )
		{
			++tally_.translated_languages[std::string( language )];
		}
		dirty_ = true;
	}

	Statistics::Day& Statistics::today()
	{
		const std::string name = log::day( std::chrono::system_clock::now() );
		tally_.days.try_emplace( name );
		// The oldest go once there are too many; never today, even with the clock set back.
		while ( tally_.days.size() > kept_days && tally_.days.begin()->first != name )
		{
			tally_.days.erase( tally_.days.begin() );
		}
		return tally_.days.find( name )->second;
	}

	void Statistics::mark( std::uint64_t& counter, std::uint64_t Day::* daily, std::uint64_t by )
	{
		if ( !enabled_ )
		{
			return;
		}
		counter += by;
		today().*daily += by;
		dirty_ = true;
	}

	void Statistics::setEnabled( bool lookups, bool translations )
	{
		const std::scoped_lock lock( mutex_ );
		enabled_              = lookups;
		translations_enabled_ = translations;
	}

	std::string Statistics::json() const
	{
		const std::scoped_lock lock( mutex_ );
		json::Writer           out;
		out.beginObject();
		out.field( "enabled", enabled_ );
		out.field( "translations_enabled", translations_enabled_ );
		out.field( "first_day", tally_.first_day );
		out.field( "days_used", static_cast<std::uint64_t>( tally_.days.size() ) );
		out.field( "sessions", tally_.sessions );
		out.field( "lookups", tally_.lookups );
		out.field( "found", tally_.found );
		out.field( "characters", tally_.characters );
		out.field( "popups", tally_.popups );
		out.field( "audio", tally_.audio );
		out.field( "anki", tally_.anki );
		out.field( "distinct_words", static_cast<std::uint64_t>( tally_.words.size() ) );
		out.field( "average_lookup_us", tally_.lookups > 0 ? static_cast<double>( tally_.microseconds ) / static_cast<double>( tally_.lookups ) : 0.0 );
		out.field( "translations", tally_.translations );
		out.field( "translated_characters", tally_.translated_characters );
		out.field( "average_translation_ms", tally_.translations_made > 0 ? static_cast<double>( tally_.translation_microseconds ) / 1000.0 / static_cast<double>( tally_.translations_made ) : 0.0 );

		// Ordered for showing: the favourites first, the days oldest first so they read as a run of days.
		out.key( "words" ).beginArray();
		for ( const auto& [word, count] : top( tally_.words, 40 ) )
		{
			out.beginObject().field( "word", word ).field( "count", count ).endObject();
		}
		out.endArray();
		out.key( "languages" ).beginArray();
		for ( const auto& [language, count] : top( tally_.languages, 16 ) )
		{
			out.beginObject().field( "language", language ).field( "count", count ).endObject();
		}
		out.endArray();
		out.key( "translated_languages" ).beginArray();
		for ( const auto& [language, count] : top( tally_.translated_languages, 16 ) )
		{
			out.beginObject().field( "language", language ).field( "count", count ).endObject();
		}
		out.endArray();
		out.key( "sources" ).beginArray();
		for ( const auto& [source, count] : top( tally_.sources, 16 ) )
		{
			out.beginObject().field( "source", source ).field( "count", count ).endObject();
		}
		out.endArray();
		out.key( "days" ).beginArray();
		for ( const auto& [name, day] : tally_.days )
		{
			out.beginObject().field( "day", name ).field( "count", day.lookups );
			for ( const auto& [field_name, field] : day_fields<Day> )
			{
				out.field( field_name, day.*field );
			}
			out.endObject();
		}
		out.endArray();
		out.endObject();
		return out.take();
	}

	void Statistics::reset()
	{
		{
			const std::scoped_lock lock( mutex_ );
			tally_           = {};
			tally_.first_day = log::day( std::chrono::system_clock::now() );
			tally_.sessions  = 1;
			dirty_           = true;
		}
		save();
		log::info( "statistics: emptied" );
	}

} // namespace lexiglance::daemon
