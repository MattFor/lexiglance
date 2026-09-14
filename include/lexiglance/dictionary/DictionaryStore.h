#ifndef LEXIGLANCE_DICTIONARY_DICTIONARYSTORE_H
#define LEXIGLANCE_DICTIONARY_DICTIONARYSTORE_H

#include <lexiglance/core/Error.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace lexiglance::dict
{

	// The directory of compiled dictionaries. File names derive from the dictionary title, which is unique.
	class DictionaryStore
	{
	public:
		explicit DictionaryStore( std::filesystem::path directory ) :
			directory_( std::move( directory ) )
		{
		}

		[[nodiscard]] const std::filesystem::path& directory() const noexcept
		{
			return directory_;
		}

		[[nodiscard]] std::filesystem::path fileFor( std::string_view title ) const;

		// Unreadable files are skipped and reported through `errors`.
		[[nodiscard]] std::vector<std::shared_ptr<const Dictionary>> loadAll( std::vector<Error>* errors = nullptr ) const;

		[[nodiscard]] Result<ImportSummary> install( const std::filesystem::path& source, const ImportOptions& options, bool replace ) const;

		[[nodiscard]] Result<> remove( std::string_view title ) const;

	private:
		std::filesystem::path directory_;
	};

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_DICTIONARYSTORE_H
