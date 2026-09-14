#ifndef LEXIGLANCE_DICTIONARY_CLASSIFY_H
#define LEXIGLANCE_DICTIONARY_CLASSIFY_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What a dictionary is for, told from its contents, and the priority order that follows: general word dictionaries
// first, then names (JMnedict), grammar, small topical ones, monolingual ones, kanji, and frequency and pitch lists.
// Dictionaries of different languages never compete: a lookup only finds words of the language it is in.
namespace lexiglance::dict
{

	class Dictionary;

	enum class Kind : std::uint8_t
	{
		Words,
		Names,
		Grammar,
		Specialized,
		Monolingual,
		Kanji,
		Frequency,
		Pitch,
		Other
	};

	// What sorting needs to know about a dictionary.
	struct Profile
	{
		std::string title;
		// As index.json says; `language` is the language of the headwords (source_language, or told from their script).
		std::string source_language;
		std::string target_language;
		std::string language;
		std::size_t terms       = 0;
		std::size_t kanji       = 0;
		std::size_t frequencies = 0;
		std::size_t pitches     = 0;
		// Shares of sampled entries: tagged as names, and with definitions written in the headwords' own script (for
		// languages not written in Latin letters): a monolingual dictionary.
		double name_share   = 0.0;
		double native_share = 0.0;
		// Share of sampled entries with a definition of their own: dictionaries explain each word, while lists of which
		// sites have an article ("pixiv | niconico") repeat a few.
		double distinct_share = 1.0;
		// Definitions in structured content (rich layout, examples), as in Jitendex.
		bool structured = false;
	};

	[[nodiscard]] Profile profile( const Dictionary& dictionary );

	// The language of a dictionary's headwords: as its index.json says, or told from their script; empty if unknown.
	[[nodiscard]] std::string dictionaryLanguage( const Dictionary& dictionary );

	[[nodiscard]] Kind classify( const Profile& profile );

	// Shown in the settings application: "Words", "Names", "Grammar", ...
	[[nodiscard]] std::string_view kindName( Kind kind ) noexcept;

	// Indices of `profiles` in their smart order. Dictionaries of one kind keep their order, except word dictionaries,
	// where well-known complete ones (Jitendex, then JMdict) lead, then richer and larger ones.
	[[nodiscard]] std::vector<std::size_t> smartOrder( std::span<const Profile> profiles );

	// Where a new dictionary goes among `existing` (in priority order): before the first one that sorts after it.
	[[nodiscard]] std::size_t insertionPoint( std::span<const Profile> existing, const Profile& added );

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_CLASSIFY_H
