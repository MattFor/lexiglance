#include <lexiglance/core/MappedFile.h>

#include <algorithm>
#include <system_error>
#include <utility>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <cerrno>
	#include <cstdio>
	#include <sys/mman.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace lexiglance
{

	MappedFile::~MappedFile()
	{
		reset();
	}

	MappedFile::MappedFile( MappedFile&& other ) noexcept :
		data_( std::exchange( other.data_, nullptr ) ),
		size_( std::exchange( other.size_, 0 ) )
#ifdef _WIN32
		,
		mapping_( std::exchange( other.mapping_, nullptr ) )
#endif
	{
	}

	MappedFile& MappedFile::operator=( MappedFile&& other ) noexcept
	{
		if ( this != &other )
		{
			reset();
			data_ = std::exchange( other.data_, nullptr );
			size_ = std::exchange( other.size_, 0 );
#ifdef _WIN32
			mapping_ = std::exchange( other.mapping_, nullptr );
#endif
		}
		return *this;
	}

#ifdef _WIN32

	Result<MappedFile> MappedFile::open( const std::filesystem::path& path )
	{
		HANDLE file = CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr );
		if ( file == INVALID_HANDLE_VALUE )
		{
			return fail( "cannot open {} (error {})", path.string(), GetLastError() );
		}

		LARGE_INTEGER size{};
		if ( !GetFileSizeEx( file, &size ) )
		{
			CloseHandle( file );
			return fail( "cannot stat {} (error {})", path.string(), GetLastError() );
		}

		MappedFile mapped;
		if ( size.QuadPart == 0 )
		{
			CloseHandle( file );
			return mapped;
		}

		HANDLE mapping = CreateFileMappingW( file, nullptr, PAGE_READONLY, 0, 0, nullptr );
		CloseHandle( file );
		if ( mapping == nullptr )
		{
			return fail( "cannot map {} (error {})", path.string(), GetLastError() );
		}

		void* view = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 );
		if ( view == nullptr )
		{
			CloseHandle( mapping );
			return fail( "cannot map {} (error {})", path.string(), GetLastError() );
		}

		mapped.data_    = view;
		mapped.size_    = static_cast<std::size_t>( size.QuadPart );
		mapped.mapping_ = mapping;
		return mapped;
	}

	void MappedFile::reset() noexcept
	{
		if ( data_ != nullptr )
		{
			UnmapViewOfFile( data_ );
		}
		if ( mapping_ != nullptr )
		{
			CloseHandle( mapping_ );
		}
		data_    = nullptr;
		mapping_ = nullptr;
		size_    = 0;
	}

	void MappedFile::prefetch( std::size_t offset, std::size_t length ) const noexcept
	{
		if ( data_ == nullptr || offset >= size_ )
		{
			return;
		}
		WIN32_MEMORY_RANGE_ENTRY range{ static_cast<char*>( data_ ) + offset, std::min( length, size_ - offset ) };
		PrefetchVirtualMemory( GetCurrentProcess(), 1, &range, 0 );
	}

	void MappedFile::adviseRandom() const noexcept {}

#else

	Result<MappedFile> MappedFile::open( const std::filesystem::path& path )
	{
		// fopen instead of the variadic ::open; "e" requests O_CLOEXEC.
		std::FILE* file = std::fopen( path.c_str(), "rbe" );
		if ( file == nullptr )
		{
			return fail( "cannot open {}: {}", path.string(), std::generic_category().message( errno ) );
		}
		const int fd = ::fileno( file );

		struct stat st{};
		if ( ::fstat( fd, &st ) != 0 )
		{
			const int err = errno;
			( void )std::fclose( file );
			return fail( "cannot stat {}: {}", path.string(), std::generic_category().message( err ) );
		}

		MappedFile mapped;
		if ( st.st_size == 0 )
		{
			( void )std::fclose( file );
			return mapped;
		}

		void*     data = ::mmap( nullptr, static_cast<std::size_t>( st.st_size ), PROT_READ, MAP_SHARED, fd, 0 );
		const int err  = errno;
		( void )std::fclose( file );

		if ( data == MAP_FAILED )
		{
			return fail( "cannot map {}: {}", path.string(), std::generic_category().message( err ) );
		}

		mapped.data_ = data;
		mapped.size_ = static_cast<std::size_t>( st.st_size );
		return mapped;
	}

	void MappedFile::reset() noexcept
	{
		if ( data_ != nullptr )
		{
			::munmap( data_, size_ );
		}
		data_ = nullptr;
		size_ = 0;
	}

	void MappedFile::prefetch( std::size_t offset, std::size_t length ) const noexcept
	{
		if ( data_ == nullptr || offset >= size_ )
		{
			return;
		}

		const auto page  = static_cast<std::size_t>( ::sysconf( _SC_PAGESIZE ) );
		const auto begin = offset / page * page;
		const auto end   = std::min( size_, offset + length );
		::madvise( static_cast<char*>( data_ ) + begin, end - begin, MADV_WILLNEED );
	}

	void MappedFile::adviseRandom() const noexcept
	{
		if ( data_ != nullptr )
		{
			::madvise( data_, size_, MADV_RANDOM );
		}
	}

#endif

#ifdef _WIN32

	namespace
	{

		// Renames a file nothing may delete while it is mapped, and deletes it if it can already.
		bool moveAside( const std::filesystem::path& path )
		{
			// InterlockedIncrement takes LONG*; tidy cannot see the write through the API.
			static LONG counter = 0; // NOLINT(misc-const-correctness)
			auto        aside   = path;
			aside += std::format( ".{}-{}.removed", GetCurrentProcessId(), InterlockedIncrement( &counter ) );
			if ( MoveFileExW( path.c_str(), aside.c_str(), 0 ) == FALSE )
			{
				return false;
			}
			DeleteFileW( aside.c_str() );
			return true;
		}

		bool inUse( DWORD error ) noexcept
		{
			return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_USER_MAPPED_FILE;
		}

	} // namespace

	Result<> removeMapped( const std::filesystem::path& path )
	{
		if ( DeleteFileW( path.c_str() ) != FALSE )
		{
			return {};
		}
		const DWORD error = GetLastError();
		if ( inUse( error ) && moveAside( path ) )
		{
			return {};
		}
		return fail( "cannot remove {} (error {})", path.string(), error );
	}

	Result<> replaceMapped( const std::filesystem::path& from, const std::filesystem::path& to )
	{
		if ( MoveFileExW( from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING ) != FALSE )
		{
			return {};
		}
		const DWORD error = GetLastError();
		if ( inUse( error ) && moveAside( to ) && MoveFileExW( from.c_str(), to.c_str(), 0 ) != FALSE )
		{
			return {};
		}
		return fail( "cannot replace {} (error {})", to.string(), error );
	}

	void sweepRemoved( const std::filesystem::path& directory )
	{
		std::error_code ec;
		for ( const auto& entry : std::filesystem::directory_iterator( directory, ec ) )
		{
			if ( entry.path().extension() == ".removed" )
			{
				// One still mapped somewhere goes next time.
				DeleteFileW( entry.path().c_str() );
			}
		}
	}

#else

	Result<> removeMapped( const std::filesystem::path& path )
	{
		std::error_code ec;
		if ( !std::filesystem::remove( path, ec ) )
		{
			return fail( "cannot remove {}: {}", path.string(), ec ? ec.message() : "it does not exist" );
		}
		return {};
	}

	Result<> replaceMapped( const std::filesystem::path& from, const std::filesystem::path& to )
	{
		std::error_code ec;
		std::filesystem::rename( from, to, ec );
		if ( ec )
		{
			return fail( "cannot replace {}: {}", to.string(), ec.message() );
		}
		return {};
	}

	void sweepRemoved( const std::filesystem::path& /*directory*/ ) {}

#endif

} // namespace lexiglance
