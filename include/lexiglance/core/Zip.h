#ifndef LEXIGLANCE_CORE_ZIP_H
#define LEXIGLANCE_CORE_ZIP_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/MappedFile.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance
{

	// Memory mapped ZIP reader (stored and deflate entries, ZIP64). `read` is const and safe to call concurrently.
	class ZipArchive
	{
	public:
		struct Entry
		{
			std::string   name;
			std::uint64_t compressed_size = 0;
			std::uint64_t size            = 0;
			std::uint64_t header_offset   = 0;
			std::uint32_t crc32           = 0;
			std::uint16_t method          = 0;
			std::uint16_t flags           = 0;
		};

		[[nodiscard]] static Result<ZipArchive> open( const std::filesystem::path& path );

		[[nodiscard]] std::span<const Entry> entries() const noexcept
		{
			return entries_;
		}

		[[nodiscard]] const Entry* find( std::string_view name ) const noexcept;

		[[nodiscard]] Result<std::string> read( const Entry& entry ) const;

	private:
		MappedFile         file_;
		std::vector<Entry> entries_;
	};

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_ZIP_H
