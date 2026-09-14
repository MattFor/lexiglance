#ifndef LEXIGLANCE_LOOKUP_DICTIONARYSET_H
#define LEXIGLANCE_LOOKUP_DICTIONARYSET_H

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/StructuredContent.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lexiglance::lookup
{

	struct LoadedDictionary
	{
		std::shared_ptr<const dict::Dictionary> dictionary;
		std::shared_ptr<const dict::StyleSheet> styles;
		std::string                             name;
	};

	// Immutable, priority ordered snapshot of the enabled dictionaries. Shared between threads through shared_ptr;
	// reloading builds a new set while lookups in flight keep using (and mapping) the old one.
	class DictionarySet
	{
	public:
		DictionarySet() = default;
		explicit DictionarySet( std::vector<LoadedDictionary> dictionaries );

		[[nodiscard]] std::span<const LoadedDictionary> all() const noexcept
		{
			return dictionaries_;
		}

		[[nodiscard]] const LoadedDictionary& operator[]( std::uint16_t index ) const noexcept
		{
			return dictionaries_[index];
		}

		[[nodiscard]] std::span<const std::uint16_t> withTerms() const noexcept
		{
			return terms_;
		}

		[[nodiscard]] std::span<const std::uint16_t> withMeta() const noexcept
		{
			return meta_;
		}

		[[nodiscard]] std::span<const std::uint16_t> withKanji() const noexcept
		{
			return kanji_;
		}

		[[nodiscard]] bool empty() const noexcept
		{
			return dictionaries_.empty();
		}

	private:
		std::vector<LoadedDictionary> dictionaries_;
		std::vector<std::uint16_t>    terms_;
		std::vector<std::uint16_t>    meta_;
		std::vector<std::uint16_t>    kanji_;
	};

} // namespace lexiglance::lookup

#endif // LEXIGLANCE_LOOKUP_DICTIONARYSET_H
