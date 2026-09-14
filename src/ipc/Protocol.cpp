#include <lexiglance/ipc/Protocol.h>

#include <lexiglance/core/Json.h>

namespace lexiglance::ipc
{

	std::string request( std::int64_t id, std::string_view method, std::string_view params_json )
	{
		json::Writer out;
		out.beginObject().field( "id", id ).field( "method", method ).key( "params" ).raw( params_json.empty() ? "{}" : params_json ).endObject();
		return out.take() + '\n';
	}

	std::string response( std::int64_t id, std::string_view result_json )
	{
		json::Writer out;
		out.beginObject().field( "id", id ).key( "result" ).raw( result_json.empty() ? "{}" : result_json ).endObject();
		return out.take() + '\n';
	}

	std::string errorResponse( std::int64_t id, std::string_view message )
	{
		json::Writer out;
		out.beginObject().field( "id", id ).key( "error" ).beginObject().field( "message", message ).endObject().endObject();
		return out.take() + '\n';
	}

	std::string event( std::string_view name, std::string_view params_json )
	{
		json::Writer out;
		out.beginObject().field( "event", name ).key( "params" ).raw( params_json.empty() ? "{}" : params_json ).endObject();
		return out.take() + '\n';
	}

	std::optional<std::string> LineBuffer::next()
	{
		const auto newline = buffer_.find( '\n', scanned_ );
		if ( newline == std::string::npos )
		{
			scanned_ = buffer_.size();
			return std::nullopt;
		}
		std::string line = buffer_.substr( 0, newline );
		buffer_.erase( 0, newline + 1 );
		scanned_ = 0;
		return line;
	}

} // namespace lexiglance::ipc
