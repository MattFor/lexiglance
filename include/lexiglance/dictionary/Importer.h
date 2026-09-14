#ifndef LEXIGLANCE_DICTIONARY_IMPORTER_H
#define LEXIGLANCE_DICTIONARY_IMPORTER_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace lexiglance::dict
{

	struct ImportProgress
	{
		std::string_view stage;
		std::uint64_t    done  = 0;
		std::uint64_t    total = 0;
	};

	struct ImportOptions
	{
		unsigned                                     threads    = 0;
		bool                                         background = true;
		std::stop_token                              stop;
		std::function<void( const ImportProgress& )> on_progress;
	};

	struct ImportSummary
	{
		std::string   title;
		std::string   revision;
		std::uint64_t terms   = 0;
		std::uint64_t meta    = 0;
		std::uint64_t kanji   = 0;
		std::uint64_t tags    = 0;
		std::uint64_t images  = 0;
		std::uint64_t skipped = 0;
		std::uint64_t bytes   = 0;
		double        seconds = 0.0;
	};

	// Title from index.json of a Yomitan dictionary (zip archive or extracted directory).
	[[nodiscard]] Result<std::string> readTitle( const std::filesystem::path& source );

	// Compiles a Yomitan dictionary into the memory mapped format; bank files are inflated and parsed in parallel.
	[[nodiscard]] Result<ImportSummary> compile( const std::filesystem::path& source, const std::filesystem::path& output, const ImportOptions& options = {} );

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_IMPORTER_H
