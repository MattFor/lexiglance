#ifndef LEXIGLANCE_TESTS_FUZZ_FUZZFILE_H
#define LEXIGLANCE_TESTS_FUZZ_FUZZFILE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace lexiglance::fuzz
{

	// Writes the input to a per-process file, for the readers that take a path (memory mapped files).
	inline std::filesystem::path write( const std::uint8_t* data, std::size_t size, const char* extension )
	{
		const auto    path = std::filesystem::temp_directory_path() / ( "lexiglance-fuzz-" + std::to_string( ::getpid() ) + extension );
		std::ofstream out( path, std::ios::binary | std::ios::trunc );
		out.write( reinterpret_cast<const char*>( data ), static_cast<std::streamsize>( size ) );
		return path;
	}

} // namespace lexiglance::fuzz

#endif // LEXIGLANCE_TESTS_FUZZ_FUZZFILE_H
