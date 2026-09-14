#ifndef LEXIGLANCE_LOOKUP_TRANSLATOR_H
#define LEXIGLANCE_LOOKUP_TRANSLATOR_H

#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/DictionarySet.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lexiglance::lookup
{

	struct LookupOptions
	{
		std::size_t max_length   = 16;
		std::size_t max_results  = 32;
		bool        search_kanji = true;
		// Only the first this many term dictionaries (in priority order) that have a match are shown; 0 shows all.
		std::size_t max_dictionaries = 0;
		// The text is exactly what the user selected (with the wheel): the result says how long the selection was.
		bool selection = false;
		// Text is looked up in the language of its script; this one wins when several share it (and takes text in no
		// language's script). nullptr: the first language.
		const lang::Language* language = nullptr;
		// The languages text is looked up in (empty: every one).
		std::vector<const lang::Language*> languages;
	};

	struct TermDefinition
	{
		std::uint16_t dictionary = 0;
		std::uint32_t term       = 0;
	};

	struct Frequency
	{
		std::uint16_t    dictionary = 0;
		std::int32_t     value      = 0;
		std::string_view display;
	};

	struct Pitch
	{
		std::uint16_t    dictionary = 0;
		std::int32_t     position   = -1;
		std::string_view pattern;
	};

	struct TermEntry
	{
		std::string_view                                         expression;
		std::string_view                                         reading;
		std::uint32_t                                            matched_length   = 0;
		std::int32_t                                             score            = 0;
		std::uint16_t                                            best_dictionary  = 0;
		bool                                                     expression_match = false;
		std::uint8_t                                             inflection_count = 0;
		std::array<std::uint16_t, lang::Deinflection::max_chain> inflections{};
		// What the matched text is of this word, as a dictionary listing inflected forms says ("genitive singular").
		std::string form_of;
		// Found only through its search key (всё for все): words spelled as written come first.
		bool folded = false;
		// A form-of entry whose word no dictionary has: shown after everything else.
		bool                        unresolved = false;
		std::vector<TermDefinition> definitions;
		std::vector<Frequency>      frequencies;
		std::vector<Pitch>          pitches;
	};

	struct KanjiEntry
	{
		std::uint16_t                    dictionary = 0;
		const dict::format::KanjiRecord* record     = nullptr;
		std::vector<Frequency>           frequencies;
	};

	struct LookupResult
	{
		std::shared_ptr<const DictionarySet> dictionaries;
		// The language the text was looked up in.
		const lang::Language* language = nullptr;
		std::string           text;
		std::uint32_t         matched_length = 0;
		// Characters selected with the wheel (0: no selection); shown when no entry spans them all.
		std::uint32_t            selected = 0;
		std::vector<TermEntry>   terms;
		std::vector<KanjiEntry>  kanji;
		std::chrono::nanoseconds elapsed{};

		[[nodiscard]] bool empty() const noexcept
		{
			return terms.empty() && kanji.empty();
		}

		[[nodiscard]] std::string_view matchedText( std::uint32_t length ) const noexcept;

		// Inflections from the dictionary form outwards, e.g. "causative « passive « past", or what a dictionary listing
		// inflected forms says of the matched text.
		[[nodiscard]] std::string inflectionText( const TermEntry& entry ) const;

		// The entry's headword as its language shows it (furigana above kanji, stress marks on Russian words).
		[[nodiscard]] std::vector<lang::RubySegment> headword( const TermEntry& entry ) const;
	};

	// Keeps the definitions of `count` dictionaries: those with the longest match first (the word the user is after),
	// then by priority (lowest index); 0 keeps all.
	void keepFirstDictionaries( LookupResult& result, std::size_t count );

	// Finds dictionary entries for the text at the start of a string, in the language of its script. Holds scratch
	// buffers, so each thread owns one.
	class Translator
	{
	public:
		[[nodiscard]] LookupResult lookup( std::shared_ptr<const DictionarySet> dictionaries, std::string_view text, const LookupOptions& options = {} );

	private:
		struct Match
		{
			std::uint16_t                                            dictionary   = 0;
			std::uint32_t                                            term         = 0;
			std::uint32_t                                            length       = 0;
			bool                                                     expression   = false;
			std::uint8_t                                             chain_length = 0;
			std::array<std::uint16_t, lang::Deinflection::max_chain> chain{};
			// Set when the match came through a form-of entry: what the text is of this word.
			std::string form_of;
			bool        folded     = false;
			bool        unresolved = false;

			// Rules applied, or the one step a form-of entry stands for; unresolved form-of entries go last.
			[[nodiscard]] std::size_t steps() const noexcept
			{
				return chain_length + ( form_of.empty() ? 0U : 1U ) + ( unresolved ? 2U : 0U );
			}
		};

		void        collect( const DictionarySet& set, const lang::TextVariant& variant, const lang::Deinflector& deinflector );
		bool        addFormsOf( const DictionarySet& set, const dict::Dictionary& dictionary, const dict::format::TermRecord& term, const Match& match, int depth = 0 );
		void        group( LookupResult& result );
		static void attachMeta( LookupResult& result );

		std::vector<lang::TextVariant>                 variants_;
		std::vector<lang::Deinflection>                deinflections_;
		std::vector<Match>                             matches_;
		std::unordered_set<std::uint64_t>              searched_;
		std::unordered_set<std::uint64_t>              seen_terms_;
		std::unordered_map<std::uint64_t, std::size_t> groups_;
	};

} // namespace lexiglance::lookup

#endif // LEXIGLANCE_LOOKUP_TRANSLATOR_H
