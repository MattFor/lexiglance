#ifndef LEXIGLANCE_DICTIONARY_SEARCHKEY_H
#define LEXIGLANCE_DICTIONARY_SEARCHKEY_H

#include <string>
#include <string_view>

// Terms are indexed under their search key too: the spelling without stress marks (кни́га -> книга), with ё written е
// (ёлка -> елка), and Greek without accents and with σ for ς (σπίτι -> σπιτι), as capitals write it. A lookup of either
// spelling then finds the term.
namespace lexiglance::dict
{

	// The search key of `text`, or an empty string when it is the text itself (the usual case, which allocates nothing).
	[[nodiscard]] std::string searchKey( std::string_view text );

	// Whether `key` is the search key of `text`, without building it.
	[[nodiscard]] bool matchesSearchKey( std::string_view text, std::string_view key ) noexcept;

	// Whether `reading` is `expression` with stress marks, as Russian dictionaries give кни́га for книга.
	[[nodiscard]] bool onlyAddsStress( std::string_view reading, std::string_view expression ) noexcept;

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_SEARCHKEY_H
