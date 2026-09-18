#include <lexiglance/core/Zip.h>

#include <algorithm>
#include <climits>
#include <fstream>
#include <iterator>
#include <span>

#include <zlib.h>

namespace lexiglance
{

	namespace
	{

		constexpr std::uint32_t local_header_signature     = 0x04034b50;
		constexpr std::uint32_t central_header_signature   = 0x02014b50;
		constexpr std::uint32_t end_of_directory_signature = 0x06054b50;
		constexpr std::uint32_t zip64_locator_signature    = 0x07064b50;
		constexpr std::uint32_t zip64_end_signature        = 0x06064b50;
		constexpr std::uint64_t max_entry_size             = 1ULL << 30U;
		// Deflate cannot expand data more than about 1032 times; a larger declared size is a lie meant to exhaust memory.
		constexpr std::uint64_t max_deflate_ratio = 1032;

		class Reader
		{
		public:
			explicit Reader( std::string_view data ) noexcept :
				data_( data )
			{
			}

			[[nodiscard]] bool has( std::uint64_t offset, std::uint64_t length ) const noexcept
			{
				return offset <= data_.size() && length <= data_.size() - offset;
			}

			template <typename T>
			[[nodiscard]] T read( std::uint64_t offset ) const noexcept
			{
				T value = 0;
				for ( std::size_t i = 0; i < sizeof( T ); ++i )
				{
					value |= static_cast<T>( static_cast<T>( static_cast<unsigned char>( data_[offset + i] ) ) << ( 8U * i ) );
				}
				return value;
			}

			[[nodiscard]] std::string_view slice( std::uint64_t offset, std::uint64_t length ) const noexcept
			{
				return data_.substr( offset, length );
			}

			[[nodiscard]] std::size_t size() const noexcept
			{
				return data_.size();
			}

		private:
			std::string_view data_;
		};

		Result<std::uint64_t> findEndOfDirectory( const Reader& reader )
		{
			constexpr std::size_t record_size = 22;
			if ( reader.size() < record_size )
			{
				return fail( "file too small to be a zip archive" );
			}

			const std::size_t lowest = reader.size() > record_size + 0xFFFF ? reader.size() - record_size - 0xFFFF : 0;
			for ( std::size_t pos = reader.size() - record_size + 1; pos-- > lowest; )
			{
				if ( reader.read<std::uint32_t>( pos ) == end_of_directory_signature )
				{
					return pos;
				}
			}
			return fail( "end of central directory not found" );
		}

		void applyZip64Extra( const Reader& reader, std::uint64_t extra, std::uint16_t extra_length, ZipArchive::Entry& entry )
		{
			std::uint64_t pos = extra;
			const auto    end = extra + extra_length;
			while ( pos + 4 <= end )
			{
				const auto id   = reader.read<std::uint16_t>( pos );
				const auto size = reader.read<std::uint16_t>( pos + 2 );
				pos += 4;
				// A field never reaches past the extra data it sits in, whatever its own size says.
				const auto limit = std::min<std::uint64_t>( pos + size, end );
				if ( id == 0x0001 )
				{
					std::uint64_t field = pos;
					if ( entry.size == 0xFFFFFFFF && field + 8 <= limit )
					{
						entry.size = reader.read<std::uint64_t>( field );
						field += 8;
					}
					if ( entry.compressed_size == 0xFFFFFFFF && field + 8 <= limit )
					{
						entry.compressed_size = reader.read<std::uint64_t>( field );
						field += 8;
					}
					if ( entry.header_offset == 0xFFFFFFFF && field + 8 <= limit )
					{
						entry.header_offset = reader.read<std::uint64_t>( field );
					}
					return;
				}
				pos += size;
			}
		}

		Result<std::string> inflateRaw( std::string_view input, std::uint64_t size )
		{
			std::string output( static_cast<std::size_t>( size ), '\0' );

			z_stream stream{};
			if ( inflateInit2( &stream, -MAX_WBITS ) != Z_OK )
			{
				return fail( "cannot initialise zlib" );
			}

			// zlib counts in uInt; feed and drain in chunks so entries above 4 GiB would still work.
			std::size_t in_pos  = 0;
			std::size_t out_pos = 0;
			int         status  = Z_OK;
			while ( status == Z_OK )
			{
				if ( stream.avail_in == 0 && in_pos < input.size() )
				{
					const auto chunk = std::min<std::size_t>( input.size() - in_pos, UINT_MAX );
					stream.next_in   = reinterpret_cast<const Bytef*>( input.data() + in_pos );
					stream.avail_in  = static_cast<uInt>( chunk );
					in_pos += chunk;
				}
				if ( stream.avail_out == 0 && out_pos < output.size() )
				{
					const auto chunk = std::min<std::size_t>( output.size() - out_pos, UINT_MAX );
					stream.next_out  = reinterpret_cast<Bytef*>( output.data() + out_pos );
					stream.avail_out = static_cast<uInt>( chunk );
					out_pos += chunk;
				}
				status = inflate( &stream, Z_NO_FLUSH );
				if ( status == Z_BUF_ERROR && stream.avail_in == 0 && in_pos >= input.size() )
				{
					break;
				}
			}

			const auto produced = stream.total_out;
			inflateEnd( &stream );

			if ( status != Z_STREAM_END || produced != size )
			{
				return fail( "corrupt deflate stream" );
			}
			return output;
		}

	} // namespace

	Result<ZipArchive> ZipArchive::open( const std::filesystem::path& path )
	{
		auto file = MappedFile::open( path );
		if ( !file )
		{
			return std::unexpected( file.error() );
		}

		ZipArchive archive;
		archive.file_ = std::move( *file );
		const Reader reader( archive.file_.view() );

		const auto eocd = findEndOfDirectory( reader );
		if ( !eocd )
		{
			return failWith( path.string(), eocd.error() );
		}

		std::uint64_t count     = reader.read<std::uint16_t>( *eocd + 10 );
		std::uint64_t directory = reader.read<std::uint32_t>( *eocd + 16 );

		if ( ( count == 0xFFFF || directory == 0xFFFFFFFF ) && *eocd >= 20 && reader.read<std::uint32_t>( *eocd - 20 ) == zip64_locator_signature )
		{
			const auto zip64 = reader.read<std::uint64_t>( *eocd - 20 + 8 );
			if ( !reader.has( zip64, 56 ) || reader.read<std::uint32_t>( zip64 ) != zip64_end_signature )
			{
				return fail( "{}: corrupt zip64 end of central directory", path.string() );
			}
			count     = reader.read<std::uint64_t>( zip64 + 32 );
			directory = reader.read<std::uint64_t>( zip64 + 48 );
		}

		archive.entries_.reserve( static_cast<std::size_t>( std::min<std::uint64_t>( count, 1U << 20U ) ) );

		std::uint64_t pos = directory;
		for ( std::uint64_t i = 0; i < count; ++i )
		{
			if ( !reader.has( pos, 46 ) || reader.read<std::uint32_t>( pos ) != central_header_signature )
			{
				return fail( "{}: corrupt central directory", path.string() );
			}

			const auto name_length    = reader.read<std::uint16_t>( pos + 28 );
			const auto extra_length   = reader.read<std::uint16_t>( pos + 30 );
			const auto comment_length = reader.read<std::uint16_t>( pos + 32 );
			if ( !reader.has( pos + 46, std::uint64_t{ name_length } + extra_length + comment_length ) )
			{
				return fail( "{}: corrupt central directory", path.string() );
			}

			Entry entry;
			entry.flags           = reader.read<std::uint16_t>( pos + 8 );
			entry.method          = reader.read<std::uint16_t>( pos + 10 );
			entry.crc32           = reader.read<std::uint32_t>( pos + 16 );
			entry.compressed_size = reader.read<std::uint32_t>( pos + 20 );
			entry.size            = reader.read<std::uint32_t>( pos + 24 );
			entry.header_offset   = reader.read<std::uint32_t>( pos + 42 );
			entry.name            = std::string( reader.slice( pos + 46, name_length ) );
			applyZip64Extra( reader, pos + 46 + name_length, extra_length, entry );

			archive.entries_.push_back( std::move( entry ) );
			pos += 46ULL + name_length + extra_length + comment_length;
		}

		return archive;
	}

	const ZipArchive::Entry* ZipArchive::find( std::string_view name ) const noexcept
	{
		const auto it = std::ranges::find( entries_, name, &Entry::name );
		return it != entries_.end() ? &*it : nullptr;
	}

	Result<std::string> ZipArchive::read( const Entry& entry ) const
	{
		const Reader reader( file_.view() );

		if ( ( entry.flags & 0x1U ) != 0 )
		{
			return fail( "{}: encrypted entries are not supported", entry.name );
		}
		if ( entry.size > max_entry_size )
		{
			return fail( "{}: entry too large", entry.name );
		}
		if ( !reader.has( entry.header_offset, 30 ) || reader.read<std::uint32_t>( entry.header_offset ) != local_header_signature )
		{
			return fail( "{}: corrupt local header", entry.name );
		}

		const auto name_length  = reader.read<std::uint16_t>( entry.header_offset + 26 );
		const auto extra_length = reader.read<std::uint16_t>( entry.header_offset + 28 );
		const auto data         = entry.header_offset + 30 + name_length + extra_length;
		if ( !reader.has( data, entry.compressed_size ) )
		{
			return fail( "{}: truncated entry", entry.name );
		}

		if ( entry.method == 8 && entry.size > ( entry.compressed_size * max_deflate_ratio ) + 1024 )
		{
			return fail( "{}: declared size impossible for its compressed size", entry.name );
		}

		const auto  input = reader.slice( data, entry.compressed_size );
		std::string output;
		switch ( entry.method )
		{
			case 0:
				output = std::string( input );
				break;
			case 8:
			{
				auto inflated = inflateRaw( input, entry.size );
				if ( !inflated )
				{
					return failWith( entry.name, inflated.error() );
				}
				output = std::move( *inflated );
				break;
			}
			default:
				return fail( "{}: unsupported compression method {}", entry.name, entry.method );
		}

		const auto checksum = crc32_z( 0, reinterpret_cast<const Bytef*>( output.data() ), output.size() );
		if ( checksum != entry.crc32 )
		{
			return fail( "{}: checksum mismatch", entry.name );
		}
		return output;
	}

	namespace
	{

		void appendLe( std::string& out, std::uint16_t value )
		{
			out.push_back( static_cast<char>( value ) );
			out.push_back( static_cast<char>( value >> 8 ) );
		}

		void appendLe( std::string& out, std::uint32_t value )
		{
			out.push_back( static_cast<char>( value ) );
			out.push_back( static_cast<char>( value >> 8 ) );
			out.push_back( static_cast<char>( value >> 16 ) );
			out.push_back( static_cast<char>( value >> 24 ) );
		}

	} // namespace

	ZipWriter::ZipWriter( std::filesystem::path path ) :
		path_( std::move( path ) )
	{
		std::error_code ec;
		std::filesystem::create_directories( path_.parent_path(), ec );
	}

	ZipWriter::~ZipWriter() = default;

	Result<> ZipWriter::add( std::string_view name, std::span<const std::byte> data )
	{
		if ( closed_ )
		{
			return fail( "zip writer is closed" );
		}
		if ( name.empty() || name.size() > 0xFFFF || data.size() > 0xFFFFFFFFu )
		{
			return fail( "zip entry name or size is out of range" );
		}
		Item item;
		item.name                = std::string( name );
		item.size                = static_cast<std::uint32_t>( data.size() );
		item.crc32               = static_cast<std::uint32_t>( crc32_z( 0, reinterpret_cast<const Bytef*>( data.data() ), data.size() ) );
		item.local_header_offset = static_cast<std::uint32_t>( buffer_.size() );

		appendLe( buffer_, local_header_signature );
		appendLe( buffer_, std::uint16_t{ 20 } );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, item.crc32 );
		appendLe( buffer_, item.size );
		appendLe( buffer_, item.size );
		appendLe( buffer_, static_cast<std::uint16_t>( item.name.size() ) );
		appendLe( buffer_, std::uint16_t{ 0 } );
		buffer_.append( item.name );
		buffer_.append( reinterpret_cast<const char*>( data.data() ), data.size() );
		items_.push_back( std::move( item ) );
		return {};
	}

	Result<> ZipWriter::addFile( std::string_view name, const std::filesystem::path& file )
	{
		std::ifstream in( file, std::ios::binary );
		if ( !in )
		{
			return fail( "cannot read {}", file.string() );
		}
		std::string data( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
		return add( name, std::as_bytes( std::span( data.data(), data.size() ) ) );
	}

	Result<> ZipWriter::close()
	{
		if ( closed_ )
		{
			return {};
		}
		closed_                   = true;
		const auto central_offset = static_cast<std::uint32_t>( buffer_.size() );
		for ( const Item& item : items_ )
		{
			appendLe( buffer_, central_header_signature );
			appendLe( buffer_, std::uint16_t{ 20 } );
			appendLe( buffer_, std::uint16_t{ 20 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, item.crc32 );
			appendLe( buffer_, item.size );
			appendLe( buffer_, item.size );
			appendLe( buffer_, static_cast<std::uint16_t>( item.name.size() ) );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint16_t{ 0 } );
			appendLe( buffer_, std::uint32_t{ 0 } );
			appendLe( buffer_, item.local_header_offset );
			buffer_.append( item.name );
		}
		const auto central_size = static_cast<std::uint32_t>( buffer_.size() ) - central_offset;
		appendLe( buffer_, end_of_directory_signature );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, std::uint16_t{ 0 } );
		appendLe( buffer_, static_cast<std::uint16_t>( items_.size() ) );
		appendLe( buffer_, static_cast<std::uint16_t>( items_.size() ) );
		appendLe( buffer_, central_size );
		appendLe( buffer_, central_offset );
		appendLe( buffer_, std::uint16_t{ 0 } );

		std::ofstream out( path_, std::ios::binary | std::ios::trunc );
		out.write( buffer_.data(), static_cast<std::streamsize>( buffer_.size() ) );
		out.close();
		buffer_.clear();
		if ( !out )
		{
			return fail( "cannot write {}", path_.string() );
		}
		return {};
	}

} // namespace lexiglance
