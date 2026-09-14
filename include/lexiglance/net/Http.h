#ifndef LEXIGLANCE_NET_HTTP_H
#define LEXIGLANCE_NET_HTTP_H

#include <lexiglance/core/Error.h>

#include <chrono>
#include <string>
#include <string_view>

namespace lexiglance::net
{

	struct Request
	{
		std::string url;
		// Sent as a POST when not empty.
		std::string               body;
		std::string               content_type = "application/json";
		std::chrono::milliseconds timeout{ 10000 };
	};

	struct Response
	{
		long        status = 0;
		std::string body;
		std::string content_type;
	};

	// Blocking; worker threads only.
	[[nodiscard]] Result<Response> fetch( const Request& request );

	[[nodiscard]] std::string urlEncode( std::string_view text );

} // namespace lexiglance::net

#endif // LEXIGLANCE_NET_HTTP_H
