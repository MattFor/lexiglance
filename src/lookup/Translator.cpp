#include <lexiglance/lookup/Translator.h>

#include <lexiglance/core/Hash.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/SearchKey.h>

#include <algorithm>
#include <tuple>

namespace lexiglance::lookup
{

	namespace
	{

		using dict::format::GlossKind;
		using dict::format::GlossRecord;
		using dict::format::TermRecord;

		// Form-of entries can point at other form-of entries (читающего -> читающий -> читать): followed this far.
		constexpr int max_form_depth = 3;

		// Rules applied, or the one step a form-of entry stands for; unresolved form-of entries go last.
		std::size_t steps( const TermEntry& entry ) noexcept
		{
			return entry.inflection_count + ( entry.form_of.empty() ? 0U : 1U ) + ( entry.unresolved ? 2U : 0U );
		}

		// Entries that only say which word they are a form of ("книги": genitive singular of книга), as Wiktionary's
		// dictionaries list every inflected form.
		bool isFormOf( const dict::Dictionary& dictionary, const TermRecord& term )
		{
			const auto glossary = dictionary.glossary( term );
			return !glossary.empty() && std::ranges::all_of( glossary, []( const GlossRecord& gloss ) { return gloss.kind == GlossKind::Deinflection; } );
		}

		struct FormOf
		{
			std::string lemma;
			std::string description;
		};

		// The words a form-of entry points at, each with what the entry is of it: ["книга", ["genitive singular"]].
		std::vector<FormOf> formsOf( const dict::Dictionary& dictionary, const TermRecord& term )
		{
			std::vector<FormOf> forms;
			for ( const GlossRecord& gloss : dictionary.glossary( term ) )
			{
				const auto document = json::Document::parse( std::string( dictionary.string( gloss.data ) ) );
				if ( !document || document->root()[0].asString().empty() )
				{
					continue;
				}
				const json::Value& root = document->root();
				std::string        description;
				for ( const json::Value& rule : root[1].items() )
				{
					if ( !rule.asString().empty() )
					{
						description.append( description.empty() ? "" : ", " ).append( rule.asString() );
					}
				}
				const std::string_view lemma = root[0].asString();
				if ( const auto it = std::ranges::find( forms, lemma, &FormOf::lemma ); it == forms.end() )
				{
					forms.push_back( { .lemma = std::string( lemma ), .description = std::move( description ) } );
				}
				else if ( !description.empty() && !it->description.contains( description ) )
				{
					it->description.append( it->description.empty() ? "" : ", " ).append( description );
				}
			}
			return forms;
		}

	} // namespace

	std::string_view LookupResult::matchedText( std::uint32_t length ) const noexcept
	{
		return utf8::prefix( text, length );
	}

	std::string LookupResult::inflectionText( const TermEntry& entry ) const
	{
		std::string out;
		if ( language != nullptr )
		{
			for ( std::size_t i = entry.inflection_count; i-- > 0; )
			{
				if ( !out.empty() )
				{
					out.append( " « " );
				}
				out.append( language->deinflector().transformName( entry.inflections[i] ) );
			}
		}
		if ( !entry.form_of.empty() )
		{
			out.append( out.empty() ? "" : " « " ).append( entry.form_of );
		}
		return out;
	}

	std::vector<lang::RubySegment> LookupResult::headword( const TermEntry& entry ) const
	{
		const lang::Language& shown = language != nullptr ? *language : lang::languageOf( entry.expression );
		return shown.headword( entry.expression, entry.reading );
	}

	void keepFirstDictionaries( LookupResult& result, std::size_t count )
	{
		if ( count == 0 )
		{
			return;
		}
		// Each dictionary with its longest match.
		std::vector<std::pair<std::uint32_t, std::uint16_t>> longest;
		for ( const TermEntry& entry : result.terms )
		{
			for ( const TermDefinition& definition : entry.definitions )
			{
				const auto it = std::ranges::find( longest, definition.dictionary, &std::pair<std::uint32_t, std::uint16_t>::second );
				if ( it == longest.end() )
				{
					longest.emplace_back( entry.matched_length, definition.dictionary );
				}
				else
				{
					it->first = std::max( it->first, entry.matched_length );
				}
			}
		}
		if ( longest.size() <= count )
		{
			return;
		}
		std::ranges::sort( longest, []( const auto& a, const auto& b ) { return std::tuple( b.first, a.second ) < std::tuple( a.first, b.second ); } );
		std::vector<std::uint16_t> found;
		found.reserve( count );
		for ( std::size_t i = 0; i < count; ++i )
		{
			found.push_back( longest[i].second );
		}
		std::ranges::sort( found );
		for ( TermEntry& entry : result.terms )
		{
			std::erase_if( entry.definitions, [&]( const TermDefinition& definition ) { return !std::ranges::binary_search( found, definition.dictionary ); } );
			if ( !entry.definitions.empty() )
			{
				entry.best_dictionary = std::ranges::min( entry.definitions, {}, &TermDefinition::dictionary ).dictionary;
			}
		}
		std::erase_if( result.terms, []( const TermEntry& entry ) { return entry.definitions.empty(); } );
	}

	LookupResult Translator::lookup( std::shared_ptr<const DictionarySet> dictionaries, std::string_view text, const LookupOptions& options )
	{
		const auto started = std::chrono::steady_clock::now();

		const lang::Language& language = lang::languageOf( text, options.language, options.languages );
		LookupResult          result;
		result.dictionaries = std::move( dictionaries );
		result.language     = &language;

		std::u32string source;
		for ( const char32_t c : utf8::codepoints( text ) )
		{
			if ( source.size() >= options.max_length || !language.isLookupCharacter( c ) )
			{
				break;
			}
			source.push_back( c );
		}
		result.text     = utf8::fromUtf32( source );
		result.selected = options.selection ? static_cast<std::uint32_t>( source.size() ) : 0;
		if ( source.empty() || !result.dictionaries || result.dictionaries->empty() )
		{
			result.elapsed = std::chrono::steady_clock::now() - started;
			return result;
		}

		variants_.clear();
		matches_.clear();
		searched_.clear();
		language.variants( source, variants_ );
		for ( const lang::TextVariant& variant : variants_ )
		{
			collect( *result.dictionaries, variant, language.deinflector() );
		}

		group( result );
		keepFirstDictionaries( result, options.max_dictionaries );

		std::ranges::stable_sort( result.terms, []( const TermEntry& a, const TermEntry& b ) {
			return std::tuple( b.matched_length, steps( a ), a.folded, !a.expression_match, b.score, a.best_dictionary ) < std::tuple( a.matched_length, steps( b ), b.folded, !b.expression_match, a.score, b.best_dictionary );
		} );
		if ( result.terms.size() > options.max_results )
		{
			result.terms.resize( options.max_results );
		}
		for ( const TermEntry& entry : result.terms )
		{
			result.matched_length = std::max( result.matched_length, entry.matched_length );
		}

		if ( options.search_kanji )
		{
			const char32_t first = source.front();
			for ( const std::uint16_t index : result.dictionaries->withKanji() )
			{
				if ( const auto* record = ( *result.dictionaries )[index].dictionary->findKanji( first ) )
				{
					result.kanji.push_back( { .dictionary = index, .record = record } );
				}
			}
		}

		attachMeta( result );
		result.elapsed = std::chrono::steady_clock::now() - started;
		return result;
	}

	void Translator::collect( const DictionarySet& set, const lang::TextVariant& variant, const lang::Deinflector& deinflector )
	{
		// Longest prefix first, so a candidate reached from several prefixes keeps the longest source span.
		for ( std::size_t k = variant.offsets.size() - 1; k > 0; --k )
		{
			const std::string_view prefix( variant.text.data(), variant.offsets[k] );
			const std::uint32_t    length = variant.source_length[k];

			deinflector.deinflect( prefix, deinflections_ );
			for ( const lang::Deinflection& candidate : deinflections_ )
			{
				const std::uint64_t key = hash64( candidate.text, candidate.conditions );
				if ( !searched_.insert( key ).second )
				{
					continue;
				}

				for ( const std::uint16_t index : set.withTerms() )
				{
					const dict::Dictionary& dictionary = *set[index].dictionary;
					const auto              terms      = dictionary.terms();
					for ( const std::uint32_t term_index : dictionary.findTerms( candidate.text ) )
					{
						if ( term_index >= terms.size() )
						{
							continue;
						}
						// Terms are also found under their search key (ёлка as елка).
						const auto& term       = terms[term_index];
						const auto  spelling   = dictionary.string( term.expression );
						bool        expression = spelling == candidate.text;
						bool        folded     = false;
						if ( !expression && dictionary.string( term.reading ) != candidate.text )
						{
							expression = dict::matchesSearchKey( spelling, candidate.text );
							if ( !expression && !dict::matchesSearchKey( dictionary.string( term.reading ), candidate.text ) )
							{
								continue;
							}
							folded = true;
						}
						if ( candidate.conditions != 0 && ( term.rules & candidate.conditions ) == 0 )
						{
							continue;
						}
						Match match{
							.dictionary   = index,
							.term         = term_index,
							.length       = length,
							.expression   = expression,
							.chain_length = candidate.length,
							.chain        = candidate.chain,
							.form_of      = {},
							.folded       = folded,
						};
						if ( !addFormsOf( set, dictionary, term, match ) )
						{
							matches_.push_back( std::move( match ) );
						}
					}
				}
			}
		}
	}

	// A form-of entry stands for the entries of the word it is a form of, in every dictionary: the dictionary listing
	// forms serves the others too. It is kept itself (last) only when no dictionary has that word.
	bool Translator::addFormsOf( const DictionarySet& set, const dict::Dictionary& dictionary, const TermRecord& term, const Match& match, int depth )
	{
		if ( !isFormOf( dictionary, term ) )
		{
			return false;
		}
		bool found = false;
		for ( const FormOf& form : formsOf( dictionary, term ) )
		{
			// What an earlier form-of entry said comes last: present active participle « genitive singular.
			const std::string description = form.description.empty() ? std::string( "inflected form" ) : form.description;
			const std::string path        = match.form_of.empty() ? description : description + " « " + match.form_of;
			// The word may be written with stress marks (кни́га); its entries are found through their search key. Its own
			// entries are what it stands for; only a word with none is followed further, as a form of yet another word.
			const std::string                                    key = dict::searchKey( form.lemma );
			std::vector<std::pair<std::uint16_t, std::uint32_t>> forms_of_it;
			bool                                                 own = false;
			for ( const std::uint16_t index : set.withTerms() )
			{
				const dict::Dictionary& other = *set[index].dictionary;
				const auto              terms = other.terms();
				for ( const std::uint32_t term_index : other.findTerms( key.empty() ? std::string_view( form.lemma ) : std::string_view( key ) ) )
				{
					if ( term_index >= terms.size() )
					{
						continue;
					}
					const auto& entry      = terms[term_index];
					const auto  expression = other.string( entry.expression );
					if ( expression != form.lemma && !dict::onlyAddsStress( form.lemma, expression ) )
					{
						continue;
					}
					if ( isFormOf( other, entry ) )
					{
						forms_of_it.emplace_back( index, term_index );
						continue;
					}
					Match resolved      = match;
					resolved.dictionary = index;
					resolved.term       = term_index;
					resolved.expression = true;
					resolved.form_of    = path;
					matches_.push_back( std::move( resolved ) );
					own = true;
				}
			}
			found = found || own;
			if ( own || depth + 1 >= max_form_depth )
			{
				continue;
			}
			for ( const auto& [index, term_index] : forms_of_it )
			{
				const dict::Dictionary& other = *set[index].dictionary;
				Match                   next  = match;
				next.form_of                  = path;
				if ( addFormsOf( set, other, other.terms()[term_index], next, depth + 1 ) )
				{
					found = true;
				}
			}
		}
		if ( !found && depth == 0 )
		{
			Match unresolved      = match;
			unresolved.unresolved = true;
			matches_.push_back( std::move( unresolved ) );
			found = true;
		}
		return found;
	}

	void Translator::group( LookupResult& result )
	{
		groups_.clear();
		seen_terms_.clear();
		const DictionarySet& set = *result.dictionaries;

		// An entry reached several ways keeps its best match: the longest, then the one with the fewest steps, what a
		// dictionary says of the form rather than a rule, and the spelling as written. Matches arrive longest first and
		// with the fewest rules first, so only form-of entries and folded spellings need sorting.
		if ( std::ranges::any_of( matches_, []( const Match& m ) { return !m.form_of.empty() || m.folded || m.unresolved; } ) )
		{
			std::ranges::stable_sort( matches_, []( const Match& a, const Match& b ) {
				return std::tuple( b.length, a.steps(), a.chain_length, a.folded, !a.expression ) < std::tuple( a.length, b.steps(), b.chain_length, b.folded, !b.expression );
			} );
		}

		for ( Match& match : matches_ )
		{
			const dict::Dictionary& dictionary = *set[match.dictionary].dictionary;
			const auto&             term       = dictionary.terms()[match.term];
			const auto              expression = dictionary.string( term.expression );
			const auto              reading    = dictionary.string( term.reading );
			const std::uint64_t     key        = hash64( reading, hash64( expression ) );

			if ( !seen_terms_.insert( ( std::uint64_t{ match.dictionary } << 32U ) | match.term ).second )
			{
				// Reached again as well: dictionaries list a form under several entries (книги: genitive singular, and
				// nominative plural), and all they say of it is kept.
				if ( const auto it = groups_.find( key ); it != groups_.end() && !match.form_of.empty() )
				{
					TermEntry& entry = result.terms[it->second];
					if ( entry.expression == expression && entry.reading == reading && entry.matched_length == match.length && steps( entry ) == match.steps() && !entry.form_of.contains( match.form_of ) )
					{
						entry.form_of.append( ", " ).append( match.form_of );
					}
				}
				continue;
			}

			auto [it, inserted] = groups_.try_emplace( key, result.terms.size() );
			if ( !inserted && ( result.terms[it->second].expression != expression || result.terms[it->second].reading != reading ) )
			{
				// 64-bit collision between two different spellings: keep them apart with a linear fallback.
				const auto same = std::ranges::find_if( result.terms, [&]( const TermEntry& e ) { return e.expression == expression && e.reading == reading; } );
				it->second      = same != result.terms.end() ? static_cast<std::size_t>( same - result.terms.begin() ) : result.terms.size();
				inserted        = same == result.terms.end();
			}
			if ( inserted )
			{
				result.terms.push_back( { .expression = expression, .reading = reading, .best_dictionary = match.dictionary } );
			}

			TermEntry& entry   = result.terms[it->second];
			const bool longer  = match.length > entry.matched_length;
			const bool simpler = match.length == entry.matched_length && match.steps() < steps( entry );
			if ( inserted || longer || simpler )
			{
				entry.matched_length   = match.length;
				entry.inflection_count = match.chain_length;
				entry.inflections      = match.chain;
				entry.expression_match = match.expression;
				entry.form_of          = std::move( match.form_of );
				entry.folded           = match.folded;
				entry.unresolved       = match.unresolved;
			}
			entry.score           = inserted ? term.score : std::max( entry.score, term.score );
			entry.best_dictionary = std::min( entry.best_dictionary, match.dictionary );
			entry.definitions.push_back( { .dictionary = match.dictionary, .term = match.term } );
		}

		for ( TermEntry& entry : result.terms )
		{
			std::ranges::stable_sort( entry.definitions, {}, &TermDefinition::dictionary );
		}
	}

	void Translator::attachMeta( LookupResult& result )
	{
		using dict::format::MetaKind;
		const DictionarySet& set = *result.dictionaries;

		for ( TermEntry& entry : result.terms )
		{
			for ( const std::uint16_t index : set.withMeta() )
			{
				const dict::Dictionary& dictionary = *set[index].dictionary;
				const auto              meta       = dictionary.meta();
				for ( const std::uint32_t meta_index : dictionary.findMeta( entry.expression ) )
				{
					if ( meta_index >= meta.size() )
					{
						continue;
					}
					const auto& record  = meta[meta_index];
					const auto  reading = dictionary.string( record.reading );
					if ( dictionary.string( record.expression ) != entry.expression || ( !reading.empty() && reading != entry.reading ) )
					{
						continue;
					}
					if ( record.kind == MetaKind::Frequency )
					{
						entry.frequencies.push_back( { .dictionary = index, .value = record.value, .display = dictionary.string( record.display ) } );
					}
					else if ( record.kind == MetaKind::Pitch && !reading.empty() )
					{
						entry.pitches.push_back( { .dictionary = index, .position = record.value, .pattern = dictionary.string( record.display ) } );
					}
				}
			}
		}

		for ( KanjiEntry& entry : result.kanji )
		{
			const auto character = ( *result.dictionaries )[entry.dictionary].dictionary->string( entry.record->character );
			for ( const std::uint16_t index : set.withMeta() )
			{
				const dict::Dictionary& dictionary = *set[index].dictionary;
				for ( const std::uint32_t meta_index : dictionary.findMeta( character ) )
				{
					const auto& record = dictionary.meta()[meta_index];
					if ( record.kind == MetaKind::KanjiFrequency && dictionary.string( record.expression ) == character )
					{
						entry.frequencies.push_back( { .dictionary = index, .value = record.value, .display = dictionary.string( record.display ) } );
					}
				}
			}
		}
	}

} // namespace lexiglance::lookup
