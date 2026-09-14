#include <lexiglance/ipc/Messages.h>

#include <lexiglance/dictionary/Classify.h>

#include <chrono>

namespace lexiglance::ipc
{

	namespace
	{

		void writeTags( json::Writer& out, const dict::Dictionary& dictionary, std::string_view tags )
		{
			out.beginArray();
			while ( !tags.empty() )
			{
				const auto space = tags.find( ' ' );
				const auto name  = tags.substr( 0, space );
				if ( !name.empty() )
				{
					const auto* tag = dictionary.findTag( name );
					out.beginObject().field( "name", name );
					out.field( "category", tag != nullptr ? dictionary.string( tag->category ) : std::string_view() );
					out.field( "notes", tag != nullptr ? dictionary.string( tag->notes ) : std::string_view() );
					out.endObject();
				}
				if ( space == std::string_view::npos )
				{
					break;
				}
				tags.remove_prefix( space + 1 );
			}
			out.endArray();
		}

		void writeFrequencies( json::Writer& out, const lookup::DictionarySet& set, const std::vector<lookup::Frequency>& frequencies )
		{
			out.beginArray();
			for ( const auto& f : frequencies )
			{
				out.beginObject().field( "dictionary", set[f.dictionary].name ).field( "value", f.value ).field( "display", f.display ).endObject();
			}
			out.endArray();
		}

	} // namespace

	void writeLookup( json::Writer& out, const lookup::LookupResult& result, dict::Markup markup )
	{
		out.beginObject();
		out.field( "text", result.text );
		out.field( "matched_length", result.matched_length );
		out.field( "elapsed_us", std::chrono::duration_cast<std::chrono::microseconds>( result.elapsed ).count() );

		out.key( "terms" ).beginArray();
		for ( const auto& entry : result.terms )
		{
			const auto& set = *result.dictionaries;
			out.beginObject();
			out.field( "expression", entry.expression );
			out.field( "reading", entry.reading );
			out.field( "matched", result.matchedText( entry.matched_length ) );
			out.field( "inflections", result.inflectionText( entry ) );

			out.key( "furigana" ).beginArray();
			for ( const auto& segment : result.headword( entry ) )
			{
				out.beginArray().value( segment.text ).value( segment.reading ).endArray();
			}
			out.endArray();

			out.key( "frequencies" );
			writeFrequencies( out, set, entry.frequencies );

			out.key( "pitches" ).beginArray();
			for ( const auto& p : entry.pitches )
			{
				out.beginObject().field( "dictionary", set[p.dictionary].name ).field( "position", p.position ).field( "pattern", p.pattern ).endObject();
			}
			out.endArray();

			out.key( "definitions" ).beginArray();
			for ( const auto& definition : entry.definitions )
			{
				const auto&               loaded     = set[definition.dictionary];
				const auto&               dictionary = *loaded.dictionary;
				const auto&               term       = dictionary.terms()[definition.term];
				const dict::MarkupOptions options{ .format = markup, .styles = loaded.styles.get() };

				out.beginObject().field( "dictionary", loaded.name );
				out.key( "tags" );
				writeTags( out, dictionary, dictionary.string( term.definition_tags ) );
				out.key( "term_tags" );
				writeTags( out, dictionary, dictionary.string( term.term_tags ) );
				out.key( "glossary" ).beginArray();
				for ( const auto& gloss : dictionary.glossary( term ) )
				{
					std::string rendered;
					dict::renderGlossary( rendered, gloss.kind, dictionary.string( gloss.data ), options );
					out.value( rendered );
				}
				out.endArray().endObject();
			}
			out.endArray();
			out.endObject();
		}
		out.endArray();

		out.key( "kanji" ).beginArray();
		for ( const auto& entry : result.kanji )
		{
			const auto& set        = *result.dictionaries;
			const auto& dictionary = *set[entry.dictionary].dictionary;
			const auto& record     = *entry.record;
			out.beginObject();
			out.field( "character", dictionary.string( record.character ) );
			out.field( "dictionary", set[entry.dictionary].name );
			out.field( "onyomi", dictionary.string( record.onyomi ) );
			out.field( "kunyomi", dictionary.string( record.kunyomi ) );
			out.key( "meanings" ).beginArray();
			for ( const auto meaning : dictionary.stringList( record.meanings_begin, record.meanings_count ) )
			{
				out.value( dictionary.string( meaning ) );
			}
			out.endArray();
			out.key( "stats" ).beginObject();
			const auto stats = dictionary.stringList( record.stats_begin, record.stats_count * 2 );
			for ( std::size_t i = 0; i + 1 < stats.size(); i += 2 )
			{
				out.field( dictionary.string( stats[i] ), dictionary.string( stats[i + 1] ) );
			}
			out.endObject();
			out.key( "frequencies" );
			writeFrequencies( out, set, entry.frequencies );
			out.endObject();
		}
		out.endArray();
		out.endObject();
	}

	void writeDictionary( json::Writer& out, const dict::Dictionary& dictionary, bool enabled )
	{
		const auto& info = dictionary.info();
		out.beginObject();
		out.field( "title", info.title );
		out.field( "revision", info.revision );
		out.field( "author", info.author );
		out.field( "url", info.url );
		out.field( "description", info.description );
		out.field( "attribution", info.attribution );
		out.field( "source_language", info.source_language );
		out.field( "target_language", info.target_language );
		out.field( "updatable", info.updatable );
		out.field( "index_url", info.index_url );
		out.field( "download_url", info.download_url );
		out.field( "enabled", enabled );
		out.field( "terms", dictionary.terms().size() );
		out.field( "meta", dictionary.meta().size() );
		out.field( "kanji", dictionary.kanji().size() );
		out.field( "size", dictionary.fileSize() );
		out.field( "import_time", dictionary.importTime() );
		out.field( "kind", dict::kindName( dict::classify( dict::profile( dictionary ) ) ) );
		out.endObject();
	}

} // namespace lexiglance::ipc
