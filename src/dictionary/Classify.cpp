#include <lexiglance/dictionary/Classify.h>

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/language/Language.h>

#include <algorithm>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace lexiglance::dict
{

	namespace
	{

		std::string lower( std::string_view text )
		{
			std::string out( text );
			std::ranges::transform( out, out.begin(), []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; } );
			return out;
		}

		bool mentions( std::string_view title, std::initializer_list<std::string_view> words )
		{
			const auto folded = lower( title );
			return std::ranges::any_of( words, [&]( std::string_view word ) { return folded.contains( word ); } );
		}

		bool latin( char32_t c ) noexcept
		{
			return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' );
		}

		// The visible text of structured content: strings in arrays and "content" values, not tag names, styles or data.
		void collectText( const json::Value& node, std::string& out, int depth = 0 )
		{
			if ( depth > 32 || out.size() > 4000 )
			{
				return;
			}
			if ( node.isString() )
			{
				out.append( node.asString() ).push_back( ' ' );
			}
			else if ( node.isArray() )
			{
				for ( const json::Value& item : node.items() )
				{
					collectText( item, out, depth + 1 );
				}
			}
			else if ( const auto* content = node.find( "content" ); node.isObject() && content != nullptr )
			{
				collectText( *content, out, depth + 1 );
			}
		}

		// What a reader sees of a definition: structured content without its markup, links and data.
		std::string visibleText( std::string_view data, bool markup )
		{
			if ( !markup )
			{
				return std::string( data );
			}
			std::string text;
			if ( auto document = json::Document::parse( std::string( data ) ) )
			{
				collectText( document->root(), text );
			}
			return text;
		}

		// Whether a definition is written in the language itself rather than explained in another one. Bilingual
		// dictionaries quote the language too (examples, readings), so its script must clearly outweigh Latin letters.
		bool writtenIn( const lang::Language& language, std::string_view text )
		{
			std::size_t own   = 0;
			std::size_t other = 0;
			for ( const char32_t c : utf8::codepoints( text ) )
			{
				own += language.isScriptCharacter( c ) ? 1 : 0;
				other += latin( c ) ? 1 : 0;
			}
			return own > 2 * other;
		}

		// The language whose script most sampled headwords start with; empty when none does.
		std::string headwordLanguage( const std::vector<std::string_view>& headwords )
		{
			std::vector<std::size_t> votes( lang::languages().size() );
			for ( const std::string_view headword : headwords )
			{
				const char32_t c = utf8::first( headword );
				for ( std::size_t i = 0; i < votes.size(); ++i )
				{
					if ( lang::languages()[i]->isScriptCharacter( c ) )
					{
						++votes[i];
						break;
					}
				}
			}
			const auto best = std::ranges::max_element( votes );
			return best == votes.end() || *best * 2 <= headwords.size() ? std::string() : std::string( lang::languages()[static_cast<std::size_t>( best - votes.begin() )]->code() );
		}

		// Sorting ranks: the kind first; among word dictionaries the well-known ones.
		int kindOrder( Kind kind ) noexcept
		{
			return static_cast<int>( kind );
		}

		int familiarity( const Profile& profile )
		{
			if ( mentions( profile.title, { "jitendex" } ) )
			{
				return 0;
			}
			if ( mentions( profile.title, { "jmdict" } ) )
			{
				return 1;
			}
			return 2;
		}

		auto sortKey( const Profile& profile )
		{
			const Kind kind  = classify( profile );
			const bool words = kind == Kind::Words;
			// Other kinds keep their order (0 for every key but the kind).
			return std::make_tuple( kindOrder( kind ), words ? familiarity( profile ) : 0, words && !profile.structured ? 1 : 0, words ? -static_cast<long long>( profile.terms ) : 0LL );
		}

	} // namespace

	Profile profile( const Dictionary& dictionary )
	{
		Profile result{
			.title           = dictionary.info().title,
			.source_language = dictionary.info().source_language,
			.target_language = dictionary.info().target_language,
			.terms           = dictionary.terms().size(),
			.kanji           = dictionary.kanji().size(),
		};

		// Meta records: a sample tells frequency lists from pitch accent ones.
		const auto meta = dictionary.meta();
		if ( !meta.empty() )
		{
			const std::size_t step = std::max<std::size_t>( 1, meta.size() / 2000 );
			std::size_t       freq = 0;
			std::size_t       seen = 0;
			for ( std::size_t i = 0; i < meta.size(); i += step, ++seen )
			{
				freq += meta[i].kind == format::MetaKind::Frequency || meta[i].kind == format::MetaKind::KanjiFrequency ? 1 : 0;
			}
			result.frequencies = meta.size() * freq / std::max<std::size_t>( 1, seen );
			result.pitches     = meta.size() - result.frequencies;
		}

		// Terms: a sample of entries tells the language, names, monolingual definitions and structured content apart.
		const auto terms = dictionary.terms();
		if ( !terms.empty() )
		{
			const std::size_t step = std::max<std::size_t>( 1, terms.size() / 400 );
			result.language        = dictionaryLanguage( dictionary );
			// Definitions in the headwords' script only tell a monolingual dictionary when that script is not Latin.
			const auto* language   = lang::findLanguage( result.language );
			const bool  own_script = language != nullptr && !language->isScriptCharacter( U'a' );
			std::size_t sampled    = 0;
			std::size_t names      = 0;
			std::size_t native     = 0;
			std::size_t structured = 0;
			// Link targets differ from entry to entry; what they say is what tells a word list.
			std::unordered_set<std::string> definitions;
			for ( std::size_t i = 0; i < terms.size(); i += step, ++sampled )
			{
				const auto&      term = terms[i];
				std::string_view tags = dictionary.string( term.definition_tags );
				bool             name = false;
				while ( !tags.empty() && !name )
				{
					const auto space = tags.find( ' ' );
					if ( const auto* tag = dictionary.findTag( tags.substr( 0, space ) ); tag != nullptr && dictionary.string( tag->category ) == "name" )
					{
						name = true;
					}
					tags = space == std::string_view::npos ? std::string_view() : tags.substr( space + 1 );
				}
				names += name ? 1 : 0;

				const auto glossary = dictionary.glossary( term );
				if ( !glossary.empty() )
				{
					const bool markup = glossary.front().kind == format::GlossKind::StructuredContent;
					structured += markup ? 1 : 0;
					auto text = visibleText( dictionary.string( glossary.front().data ), markup );
					native += own_script && writtenIn( *language, text ) ? 1 : 0;
					definitions.insert( std::move( text ) );
				}
			}
			result.name_share     = static_cast<double>( names ) / static_cast<double>( sampled );
			result.native_share   = static_cast<double>( native ) / static_cast<double>( sampled );
			result.distinct_share = static_cast<double>( definitions.size() ) / static_cast<double>( sampled );
			result.structured     = structured * 2 > sampled;
		}
		return result;
	}

	std::string dictionaryLanguage( const Dictionary& dictionary )
	{
		if ( const auto* declared = lang::findLanguage( lower( dictionary.info().source_language ) ) )
		{
			return std::string( declared->code() );
		}
		const auto                    terms = dictionary.terms();
		const std::size_t             step  = std::max<std::size_t>( 1, terms.size() / 64 );
		std::vector<std::string_view> headwords;
		for ( std::size_t i = 0; i < terms.size(); i += step )
		{
			headwords.push_back( dictionary.string( terms[i].expression ) );
		}
		return headwordLanguage( headwords );
	}

	Kind classify( const Profile& profile )
	{
		if ( profile.terms == 0 )
		{
			if ( profile.kanji > 0 )
			{
				return Kind::Kanji;
			}
			if ( profile.frequencies > 0 && profile.frequencies >= profile.pitches )
			{
				return Kind::Frequency;
			}
			return profile.pitches > 0 ? Kind::Pitch : Kind::Other;
		}
		if ( profile.name_share > 0.5 || mentions( profile.title, { "jmnedict", "names", "人名", "名前" } ) )
		{
			return Kind::Names;
		}
		if ( mentions( profile.title, { "文型", "文法", "grammar", "dojg" } ) )
		{
			return Kind::Grammar;
		}
		// The complete Japanese-English dictionaries are what everyone has; no sample decides otherwise.
		if ( mentions( profile.title, { "jitendex", "jmdict" } ) )
		{
			return Kind::Words;
		}
		if ( const auto target = lower( profile.target_language ); ( !target.empty() && target == lower( profile.source_language.empty() ? profile.language : profile.source_language ) ) || profile.native_share > 0.5 )
		{
			return Kind::Monolingual;
		}
		// Small word lists cover one subject (onomatopoeia, idioms, slang), and lists that repeat a few definitions only
		// say where a word appears: both after the general dictionaries.
		return profile.terms < 50000 || profile.distinct_share < 0.2 ? Kind::Specialized : Kind::Words;
	}

	std::string_view kindName( Kind kind ) noexcept
	{
		switch ( kind )
		{
			case Kind::Words:
				return "Words";
			case Kind::Names:
				return "Names";
			case Kind::Grammar:
				return "Grammar";
			case Kind::Specialized:
				return "Specialized";
			case Kind::Monolingual:
				return "Monolingual";
			case Kind::Kanji:
				return "Kanji";
			case Kind::Frequency:
				return "Frequency";
			case Kind::Pitch:
				return "Pitch accent";
			case Kind::Other:
				break;
		}
		return "Other";
	}

	std::vector<std::size_t> smartOrder( std::span<const Profile> profiles )
	{
		std::vector<std::size_t> order( profiles.size() );
		std::ranges::iota( order, std::size_t{ 0 } );
		std::vector<decltype( sortKey( Profile{} ) )> keys;
		keys.reserve( profiles.size() );
		for ( const auto& profile : profiles )
		{
			keys.push_back( sortKey( profile ) );
		}
		std::ranges::stable_sort( order, {}, [&]( std::size_t index ) { return keys[index]; } );
		return order;
	}

	std::size_t insertionPoint( std::span<const Profile> existing, const Profile& added )
	{
		const auto key = sortKey( added );
		for ( std::size_t i = 0; i < existing.size(); ++i )
		{
			if ( key < sortKey( existing[i] ) )
			{
				return i;
			}
		}
		return existing.size();
	}

} // namespace lexiglance::dict
