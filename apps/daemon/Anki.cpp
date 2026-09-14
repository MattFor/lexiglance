#include "Anki.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/dictionary/StructuredContent.h>
#include <lexiglance/net/Http.h>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <vector>

namespace lexiglance::daemon
{

	namespace
	{

		std::string escapeHtml( std::string_view text )
		{
			std::string out;
			dict::escapeMarkup( out, text, dict::Markup::Html );
			return out;
		}

		// "JMdict [2026-09-12]" -> "JMdict".
		std::string shortName( std::string_view title )
		{
			if ( const auto bracket = title.find( " [" ); bracket != std::string_view::npos && title.ends_with( ']' ) )
			{
				title = title.substr( 0, bracket );
			}
			return std::string( title );
		}

		bool isNumber( std::string_view text )
		{
			return !text.empty() && std::ranges::all_of( text, []( char c ) { return c >= '0' && c <= '9'; } );
		}

		std::string joined( const std::vector<std::string>& parts, std::string_view separator )
		{
			std::string out;
			for ( const auto& part : parts )
			{
				if ( !out.empty() )
				{
					out.append( separator );
				}
				out.append( part );
			}
			return out;
		}

		// One definition as a list item: "(tags, dictionary)" followed by its glosses.
		std::string definitionHtml( const lookup::DictionarySet& set, const lookup::TermDefinition& definition )
		{
			const auto&               loaded     = set[definition.dictionary];
			const auto&               dictionary = *loaded.dictionary;
			const auto&               term       = dictionary.terms()[definition.term];
			const dict::MarkupOptions options{ .format = dict::Markup::Html, .styles = loaded.styles.get() };

			std::vector<std::string> labels;
			for ( const auto tag : std::views::split( dictionary.string( term.definition_tags ), ' ' ) )
			{
				const std::string_view name( tag.begin(), tag.end() );
				if ( !name.empty() && !isNumber( name ) )
				{
					labels.emplace_back( name );
				}
			}
			labels.push_back( shortName( loaded.name ) );

			std::vector<std::string> glosses;
			for ( const auto& gloss : dictionary.glossary( term ) )
			{
				std::string rendered;
				dict::renderGlossary( rendered, gloss.kind, dictionary.string( gloss.data ), options );
				glosses.push_back( std::move( rendered ) );
			}

			std::string out = std::format( "<i>({})</i> ", escapeHtml( joined( labels, ", " ) ) );
			if ( glosses.size() == 1 )
			{
				out.append( glosses.front() );
				return out;
			}
			out.append( "<ul>" );
			for ( const auto& gloss : glosses )
			{
				out.append( "<li>" ).append( gloss ).append( "</li>" );
			}
			out.append( "</ul>" );
			return out;
		}

		std::string glossaryHtml( const lookup::LookupResult& result, const lookup::TermEntry& entry, std::size_t limit )
		{
			std::vector<std::string> items;
			for ( const auto& definition : entry.definitions )
			{
				if ( items.size() >= limit )
				{
					break;
				}
				items.push_back( definitionHtml( *result.dictionaries, definition ) );
			}
			if ( items.size() == 1 )
			{
				return std::format( "<div style=\"text-align: left;\">{}</div>", items.front() );
			}
			std::string out = "<div style=\"text-align: left;\"><ol>";
			for ( const auto& item : items )
			{
				out.append( "<li>" ).append( item ).append( "</li>" );
			}
			return out.append( "</ol></div>" );
		}

		// Anki's furigana syntax: 食[た]べる, with a space before each annotated part that follows other text.
		std::string furiganaPlain( const std::vector<lang::RubySegment>& segments )
		{
			std::string out;
			for ( const auto& segment : segments )
			{
				if ( segment.reading.empty() )
				{
					out.append( segment.text );
					continue;
				}
				if ( !out.empty() && out.back() != ' ' )
				{
					out.push_back( ' ' );
				}
				out.append( std::format( "{}[{}]", segment.text, segment.reading ) );
			}
			return out;
		}

		std::string furiganaHtml( const std::vector<lang::RubySegment>& segments )
		{
			std::string out;
			for ( const auto& segment : segments )
			{
				if ( segment.reading.empty() )
				{
					out.append( escapeHtml( segment.text ) );
				}
				else
				{
					out.append( std::format( "<ruby>{}<rt>{}</rt></ruby>", escapeHtml( segment.text ), escapeHtml( segment.reading ) ) );
				}
			}
			return out;
		}

		std::string friendlyError( std::string_view message )
		{
			if ( message.contains( "duplicate" ) )
			{
				return "Already in Anki";
			}
			if ( message.contains( "Couldn't connect" ) || message.contains( "Could not connect" ) || message.contains( "Connection refused" ) )
			{
				return "Anki is not running (AnkiConnect)";
			}
			if ( message.contains( "deck was not found" ) )
			{
				return "Anki deck not found";
			}
			if ( message.contains( "model was not found" ) )
			{
				return "Anki note type not found";
			}
			return std::string( message );
		}

		// Sends an AnkiConnect request; the reply's "error" becomes a friendly error.
		Result<json::Document> ankiRequest( const config::AnkiSettings& settings, std::string body, std::chrono::milliseconds timeout )
		{
			auto response = net::fetch( { .url = settings.url, .body = std::move( body ), .timeout = timeout } );
			if ( !response )
			{
				return fail( "{}", friendlyError( response.error().message ) );
			}
			auto document = json::Document::parse( std::move( response->body ) );
			if ( !document || !document->root().isObject() )
			{
				return fail( "Unexpected reply from AnkiConnect (HTTP {})", response->status );
			}
			if ( const auto& error = document->root()["error"]; error.isString() )
			{
				return fail( "{}", friendlyError( error.asString() ) );
			}
			return document;
		}

		void beginRequest( json::Writer& out, std::string_view action, const config::AnkiSettings& settings )
		{
			out.beginObject().field( "action", action ).field( "version", 6 );
			if ( !settings.key.empty() )
			{
				out.field( "key", settings.key );
			}
		}

	} // namespace

	Result<std::int64_t> ankiVersion( const config::AnkiSettings& settings )
	{
		json::Writer out;
		beginRequest( out, "version", settings );
		out.endObject();
		const auto document = ankiRequest( settings, out.take(), std::chrono::milliseconds( 1500 ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		return document->root()["result"].asInt();
	}

	std::pair<std::string, std::size_t> sentenceAround( std::string_view text, std::size_t offset )
	{
		static constexpr std::array<std::string_view, 7> terminators{ "。", "！", "？", "!", "?", "\n", "．" };
		offset = std::min( offset, text.size() );

		std::size_t begin = 0;
		std::size_t end   = text.size();
		for ( const auto terminator : terminators )
		{
			if ( const auto before = text.substr( 0, offset ).rfind( terminator ); before != std::string_view::npos )
			{
				begin = std::max( begin, before + terminator.size() );
			}
			if ( const auto after = text.find( terminator, offset ); after != std::string_view::npos )
			{
				end = std::min( end, terminator == "\n" ? after : after + terminator.size() );
			}
		}
		while ( begin < offset && ( text[begin] == ' ' || text[begin] == '\t' ) )
		{
			++begin;
		}
		return { std::string( text.substr( begin, end - begin ) ), offset - begin };
	}

	Markers noteMarkers( const lookup::LookupResult& result, const lookup::TermEntry& entry, const NoteContext& context )
	{
		Markers           markers;
		const std::string expression( entry.expression );
		const std::string reading  = entry.reading.empty() ? expression : std::string( entry.reading );
		const auto        segments = result.headword( entry );
		const auto&       set      = *result.dictionaries;

		markers["expression"]     = expression;
		markers["reading"]        = reading;
		markers["furigana"]       = furiganaHtml( segments );
		markers["furigana-plain"] = furiganaPlain( segments );
		markers["glossary"]       = glossaryHtml( result, entry, entry.definitions.size() );
		markers["glossary-first"] = glossaryHtml( result, entry, 1 );
		markers["dictionary"]     = entry.definitions.empty() ? std::string() : shortName( set[entry.definitions.front().dictionary].name );
		markers["audio"]          = "";

		if ( !entry.definitions.empty() )
		{
			const auto& front      = entry.definitions.front();
			const auto& dictionary = *set[front.dictionary].dictionary;
			std::string tags( dictionary.string( dictionary.terms()[front.term].definition_tags ) );
			std::ranges::replace( tags, ' ', ',' );
			markers["part-of-speech"] = tags;
		}

		std::vector<std::string> frequencies;
		frequencies.reserve( entry.frequencies.size() );
		for ( const auto& frequency : entry.frequencies )
		{
			frequencies.push_back( std::format( "{}: {}", shortName( set[frequency.dictionary].name ), frequency.display.empty() ? std::to_string( frequency.value ) : std::string( frequency.display ) ) );
		}
		markers["frequencies"] = joined( frequencies, ", " );

		std::vector<std::string> pitches;
		std::vector<std::string> positions;
		for ( const auto& pitch : entry.pitches )
		{
			if ( pitch.position >= 0 && std::ranges::find( positions, std::to_string( pitch.position ) ) == positions.end() )
			{
				positions.push_back( std::to_string( pitch.position ) );
				pitches.push_back( std::format( "{} [{}]", reading, pitch.position ) );
			}
		}
		markers["pitch-accents"]          = joined( pitches, ", " );
		markers["pitch-accent-positions"] = joined( positions, ", " );

		// Sentence and cloze parts around the matched text.
		const std::string_view matched = result.matchedText( entry.matched_length );
		auto [sentence, offset]        = sentenceAround( context.sentence, context.sentence_offset );
		if ( !std::string_view( sentence ).substr( offset ).starts_with( matched ) )
		{
			offset = sentence.find( matched );
		}
		markers["sentence"] = sentence;
		if ( offset != std::string::npos && !matched.empty() )
		{
			markers["cloze-prefix"] = sentence.substr( 0, offset );
			markers["cloze-body"]   = std::string( matched );
			markers["cloze-suffix"] = sentence.substr( offset + matched.size() );
		}
		else
		{
			markers["cloze-prefix"] = "";
			markers["cloze-body"]   = std::string( matched );
			markers["cloze-suffix"] = "";
		}
		return markers;
	}

	std::string fillTemplate( std::string_view pattern, const Markers& markers )
	{
		std::string out;
		out.reserve( pattern.size() );
		std::size_t at = 0;
		while ( at < pattern.size() )
		{
			const auto open  = pattern.find( '{', at );
			const auto close = open == std::string_view::npos ? std::string_view::npos : pattern.find( '}', open );
			if ( close == std::string_view::npos )
			{
				out.append( pattern.substr( at ) );
				break;
			}
			out.append( pattern.substr( at, open - at ) );
			const std::string name( pattern.substr( open + 1, close - open - 1 ) );
			if ( const auto it = markers.find( name ); it != markers.end() )
			{
				out.append( it->second );
			}
			else
			{
				out.append( pattern.substr( open, close - open + 1 ) );
			}
			at = close + 1;
		}
		return out;
	}

	Result<std::vector<bool>> canAddNotes( const config::AnkiSettings& settings, const lookup::LookupResult& result, const NoteContext& context )
	{
		if ( settings.deck.empty() || settings.model.empty() || settings.fields.empty() )
		{
			return fail( "Anki is not set up" );
		}
		// AnkiConnect judges duplicates by the first field only.
		const auto& first = settings.fields.front();
		const auto  count = std::min<std::size_t>( result.terms.size(), 24 );

		json::Writer out( false );
		beginRequest( out, "canAddNotes", settings );
		out.key( "params" ).beginObject().key( "notes" ).beginArray();
		for ( std::size_t i = 0; i < count; ++i )
		{
			out.beginObject().field( "deckName", settings.deck ).field( "modelName", settings.model );
			out.key( "fields" ).beginObject().field( first.name, fillTemplate( first.value, noteMarkers( result, result.terms[i], context ) ) ).endObject();
			out.key( "options" ).beginObject().field( "allowDuplicate", false ).field( "duplicateScope", "deck" ).endObject();
			out.endObject();
		}
		out.endArray().endObject().endObject();

		const auto document = ankiRequest( settings, out.take(), std::chrono::seconds( 3 ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		std::vector<bool> addable;
		for ( const auto& item : document->root()["result"].items() )
		{
			addable.push_back( item.asBool( true ) );
		}
		return addable;
	}

	Result<std::int64_t> addNote( const config::AnkiSettings& settings, const lookup::LookupResult& result, const lookup::TermEntry& entry, const NoteContext& context )
	{
		if ( settings.deck.empty() || settings.model.empty() || settings.fields.empty() )
		{
			return fail( "Choose a deck and note type in Settings -> Anki" );
		}
		const Markers markers = noteMarkers( result, entry, context );

		json::Writer out( false );
		beginRequest( out, "addNote", settings );
		out.key( "params" ).beginObject().key( "note" ).beginObject();
		out.field( "deckName", settings.deck ).field( "modelName", settings.model );

		std::vector<std::string> audio_fields;
		out.key( "fields" ).beginObject();
		for ( const auto& field : settings.fields )
		{
			if ( field.value.contains( "{audio}" ) )
			{
				audio_fields.push_back( field.name );
			}
			out.field( field.name, fillTemplate( field.value, markers ) );
		}
		out.endObject();

		out.key( "options" ).beginObject().field( "allowDuplicate", settings.allow_duplicates ).field( "duplicateScope", "deck" ).endObject();
		out.key( "tags" ).beginArray();
		for ( const auto& tag : settings.tags )
		{
			out.value( tag );
		}
		out.endArray();

		// AnkiConnect downloads the clip itself and stores it in the collection's media.
		if ( context.audio && !audio_fields.empty() )
		{
			std::string filename = std::format( "lexiglance_{}_{}.mp3", markers.at( "expression" ), markers.at( "reading" ) );
			std::ranges::replace( filename, '/', '_' );
			out.key( "audio" ).beginArray().beginObject();
			out.field( "url", context.audio->url ).field( "filename", filename );
			if ( !context.audio->placeholder_hash.empty() )
			{
				out.field( "skipHash", context.audio->placeholder_hash );
			}
			out.key( "fields" ).beginArray();
			for ( const auto& field : audio_fields )
			{
				out.value( field );
			}
			out.endArray().endObject().endArray();
		}
		out.endObject().endObject().endObject();

		const auto document = ankiRequest( settings, out.take(), std::chrono::seconds( 20 ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		return document->root()["result"].asInt();
	}

} // namespace lexiglance::daemon
