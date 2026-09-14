#ifndef LEXIGLANCE_DICTIONARY_FORMAT_H
#define LEXIGLANCE_DICTIONARY_FORMAT_H

#include <lexiglance/core/Hash.h>

#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <type_traits>

// Compiled dictionary (*.lgd) layout, little endian, every section 8-byte aligned:
//   Header | string pool | terms | glosses | term index | term postings | meta | meta index | meta postings | kanji | tags | string lists
// Index sections are open addressing hash tables (IndexSlot) whose slots point at runs in the matching postings array.
namespace lexiglance::dict::format
{

	static_assert( std::endian::native == std::endian::little, "compiled dictionaries are little endian" );

	inline constexpr std::array<char, 8> magic     = { 'L', 'X', 'G', 'L', 'D', 'I', 'C', 'T' };
	inline constexpr std::uint32_t       version   = 1;
	inline constexpr std::string_view    extension = ".lgd";

	struct StrRef
	{
		std::uint32_t offset = 0;
		std::uint32_t length = 0;
	};

	struct Section
	{
		std::uint64_t offset = 0;
		std::uint64_t count  = 0;
	};

	enum class GlossKind : std::uint8_t
	{
		Text,
		StructuredContent,
		Image,
		Deinflection
	};

	enum class MetaKind : std::uint8_t
	{
		Frequency,
		Pitch,
		Ipa,
		KanjiFrequency
	};

	struct TermRecord
	{
		StrRef        expression;
		StrRef        reading;
		StrRef        definition_tags;
		StrRef        term_tags;
		StrRef        rules_text;
		std::uint32_t rules       = 0;
		std::int32_t  score       = 0;
		std::uint32_t sequence    = 0;
		std::uint32_t gloss_begin = 0;
		std::uint32_t gloss_count = 0;
		std::uint32_t reserved    = 0;
	};

	struct GlossRecord
	{
		StrRef                      data;
		GlossKind                   kind = GlossKind::Text;
		std::array<std::uint8_t, 3> reserved{};
	};

	struct IndexSlot
	{
		std::uint64_t hash  = 0;
		std::uint32_t begin = 0;
		std::uint32_t count = 0;
	};

	struct MetaRecord
	{
		StrRef                      expression;
		StrRef                      reading;
		StrRef                      display;
		StrRef                      extra;
		std::int32_t                value = 0;
		MetaKind                    kind  = MetaKind::Frequency;
		std::array<std::uint8_t, 3> reserved{};
	};

	struct KanjiRecord
	{
		std::uint32_t codepoint = 0;
		StrRef        character;
		StrRef        onyomi;
		StrRef        kunyomi;
		StrRef        tags;
		std::uint32_t meanings_begin = 0;
		std::uint32_t meanings_count = 0;
		std::uint32_t stats_begin    = 0;
		std::uint32_t stats_count    = 0;
		std::uint32_t reserved       = 0;
	};

	struct TagRecord
	{
		StrRef       name;
		StrRef       category;
		StrRef       notes;
		std::int32_t order = 0;
		std::int32_t score = 0;
	};

	struct Header
	{
		std::array<char, 8>          magic{};
		std::uint32_t                version     = 0;
		std::uint32_t                header_size = 0;
		std::uint64_t                file_size   = 0;
		std::uint64_t                import_time = 0;
		Section                      pool;
		Section                      terms;
		Section                      glosses;
		Section                      term_index;
		Section                      term_postings;
		Section                      meta;
		Section                      meta_index;
		Section                      meta_postings;
		Section                      kanji;
		Section                      tags;
		Section                      string_lists;
		StrRef                       index_json;
		StrRef                       styles_css;
		std::array<std::uint64_t, 8> reserved{};
	};

	static_assert( sizeof( StrRef ) == 8 );
	static_assert( sizeof( TermRecord ) == 64 );
	static_assert( sizeof( GlossRecord ) == 12 );
	static_assert( sizeof( IndexSlot ) == 16 );
	static_assert( sizeof( MetaRecord ) == 40 );
	static_assert( sizeof( KanjiRecord ) == 56 );
	static_assert( sizeof( TagRecord ) == 32 );
	static_assert( sizeof( Header ) == 288 );
	static_assert( std::is_trivially_copyable_v<Header> && std::is_trivially_copyable_v<TermRecord> );

	// Zero marks an empty slot, so it is never produced as a key hash.
	[[nodiscard]] constexpr std::uint64_t indexHash( std::string_view key ) noexcept
	{
		const auto h = hash64( key );
		return h == 0 ? 1 : h;
	}

} // namespace lexiglance::dict::format

#endif // LEXIGLANCE_DICTIONARY_FORMAT_H
