#ifndef LEXIGLANCE_DICTIONARY_DICTIONARY_H
#define LEXIGLANCE_DICTIONARY_DICTIONARY_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/MappedFile.h>
#include <lexiglance/dictionary/Format.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lexiglance::dict
{

	struct DictionaryInfo
	{
		std::string title;
		std::string revision;
		std::string author;
		std::string url;
		std::string description;
		std::string attribution;
		std::string source_language;
		std::string target_language;
		std::string frequency_mode;
		std::string index_url;
		std::string download_url;
		int         format    = 3;
		bool        sequenced = false;
		bool        updatable = false;
	};

	// Read-only view of a compiled dictionary. All returned views point into the mapping and live as long as the object.
	class Dictionary
	{
	public:
		[[nodiscard]] static Result<std::shared_ptr<const Dictionary>> open( const std::filesystem::path& path );

		[[nodiscard]] const DictionaryInfo& info() const noexcept
		{
			return info_;
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

		[[nodiscard]] std::uint64_t fileSize() const noexcept
		{
			return file_.size();
		}

		[[nodiscard]] std::uint64_t importTime() const noexcept
		{
			return header_->import_time;
		}

		[[nodiscard]] std::string_view string( format::StrRef ref ) const noexcept;

		[[nodiscard]] std::span<const format::TermRecord> terms() const noexcept
		{
			return terms_;
		}

		[[nodiscard]] std::span<const format::MetaRecord> meta() const noexcept
		{
			return meta_;
		}

		[[nodiscard]] std::span<const format::KanjiRecord> kanji() const noexcept
		{
			return kanji_;
		}

		[[nodiscard]] std::span<const format::TagRecord> tags() const noexcept
		{
			return tags_;
		}

		// Indices of terms whose expression or reading hashes like `key`; callers compare the strings.
		[[nodiscard]] std::span<const std::uint32_t> findTerms( std::string_view key ) const noexcept;

		[[nodiscard]] std::span<const std::uint32_t> findMeta( std::string_view expression ) const noexcept;

		[[nodiscard]] std::span<const format::GlossRecord> glossary( const format::TermRecord& term ) const noexcept;

		[[nodiscard]] const format::KanjiRecord* findKanji( char32_t codepoint ) const noexcept;

		[[nodiscard]] const format::TagRecord* findTag( std::string_view name ) const noexcept;

		[[nodiscard]] std::span<const format::StrRef> stringList( std::uint32_t begin, std::uint32_t count ) const noexcept;

		[[nodiscard]] std::string_view indexJson() const noexcept
		{
			return string( header_->index_json );
		}

		[[nodiscard]] std::string_view styles() const noexcept
		{
			return string( header_->styles_css );
		}

		// Starts background read-ahead of the hash indexes so that the first lookups do not page fault.
		void prefetchIndexes() const noexcept;

	private:
		Dictionary() = default;

		[[nodiscard]] static std::span<const std::uint32_t>
		probe( std::span<const format::IndexSlot> index, std::span<const std::uint32_t> postings, std::string_view key ) noexcept;

		MappedFile                           file_;
		std::filesystem::path                path_;
		DictionaryInfo                       info_;
		const format::Header*                header_ = nullptr;
		std::string_view                     pool_;
		std::span<const format::TermRecord>  terms_;
		std::span<const format::GlossRecord> glosses_;
		std::span<const format::IndexSlot>   term_index_;
		std::span<const std::uint32_t>       term_postings_;
		std::span<const format::MetaRecord>  meta_;
		std::span<const format::IndexSlot>   meta_index_;
		std::span<const std::uint32_t>       meta_postings_;
		std::span<const format::KanjiRecord> kanji_;
		std::span<const format::TagRecord>   tags_;
		std::span<const format::StrRef>      string_lists_;
	};

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_DICTIONARY_H
