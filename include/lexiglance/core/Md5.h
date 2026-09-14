#ifndef LEXIGLANCE_CORE_MD5_H
#define LEXIGLANCE_CORE_MD5_H

#include <string>
#include <string_view>

namespace lexiglance
{

	// RFC 1321, as lowercase hex. Only for recognising known files (e.g. audio placeholders), never for security.
	[[nodiscard]] std::string md5Hex( std::string_view data );

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_MD5_H
