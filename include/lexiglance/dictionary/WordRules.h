#ifndef LEXIGLANCE_DICTIONARY_WORDRULES_H
#define LEXIGLANCE_DICTIONARY_WORDRULES_H

#include <cstdint>
#include <string_view>

// Word classes named by the `rules` field of Yomitan term banks, as bits. Bits 0-23 are dictionary word classes,
// bits 24-31 are reserved for intermediate conditions private to a language's deinflector.
namespace lexiglance::dict::rule
{

	// Japanese conjugation classes (JMdict's names).
	inline constexpr std::uint32_t v1    = 1U << 0U;
	inline constexpr std::uint32_t v5    = 1U << 1U;
	inline constexpr std::uint32_t vs    = 1U << 2U;
	inline constexpr std::uint32_t vk    = 1U << 3U;
	inline constexpr std::uint32_t vz    = 1U << 4U;
	inline constexpr std::uint32_t adj_i = 1U << 5U;

	// Parts of speech, for languages whose dictionaries name them plainly (Wiktionary's: n, v, adj).
	inline constexpr std::uint32_t noun      = 1U << 6U;
	inline constexpr std::uint32_t verb      = 1U << 7U;
	inline constexpr std::uint32_t adjective = 1U << 8U;

	// Classes also taken from a term's definition tags: dictionaries such as JMdict mark suru nouns only there, and
	// Wiktionary's give the part of speech only there.
	inline constexpr std::uint32_t from_tags = vs | noun | verb | adjective;

	inline constexpr std::uint32_t dictionary_mask = 0x00FFFFFFU;

	[[nodiscard]] std::uint32_t parse( std::string_view rules ) noexcept;

} // namespace lexiglance::dict::rule

#endif // LEXIGLANCE_DICTIONARY_WORDRULES_H
