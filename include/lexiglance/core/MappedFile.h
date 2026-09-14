#ifndef LEXIGLANCE_CORE_MAPPEDFILE_H
#define LEXIGLANCE_CORE_MAPPEDFILE_H

#include <lexiglance/core/Error.h>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace lexiglance
{

	// Read-only mapping of a whole file. Opening is O(1) and only pages touched by lookups are ever read.
	class MappedFile
	{
	public:
		MappedFile() noexcept = default;
		~MappedFile();

		MappedFile( const MappedFile& )            = delete;
		MappedFile& operator=( const MappedFile& ) = delete;

		MappedFile( MappedFile&& other ) noexcept;
		MappedFile& operator=( MappedFile&& other ) noexcept;

		[[nodiscard]] static Result<MappedFile> open( const std::filesystem::path& path );

		[[nodiscard]] const char* data() const noexcept
		{
			return static_cast<const char*>( data_ );
		}

		[[nodiscard]] std::size_t size() const noexcept
		{
			return size_;
		}

		[[nodiscard]] std::string_view view() const noexcept
		{
			return { data(), size_ };
		}

		// Non-blocking kernel read-ahead of a byte range.
		void prefetch( std::size_t offset, std::size_t length ) const noexcept;

		void adviseRandom() const noexcept;

	private:
		void reset() noexcept;

		void*       data_ = nullptr;
		std::size_t size_ = 0;
#ifdef _WIN32
		void* mapping_ = nullptr;
#endif
	};

	// Removing or replacing a file that may be mapped (a dictionary the daemon has open). Windows refuses to delete a
	// mapped file but lets it be renamed: it is moved aside under a ".removed" name and deleted once nothing maps it any
	// more, by sweepRemoved(). Elsewhere these are a plain remove and rename.
	[[nodiscard]] Result<> removeMapped( const std::filesystem::path& path );
	[[nodiscard]] Result<> replaceMapped( const std::filesystem::path& from, const std::filesystem::path& to );
	void                   sweepRemoved( const std::filesystem::path& directory );

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_MAPPEDFILE_H
