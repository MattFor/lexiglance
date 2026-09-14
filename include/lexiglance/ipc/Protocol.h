#ifndef LEXIGLANCE_IPC_PROTOCOL_H
#define LEXIGLANCE_IPC_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Daemon <-> client protocol: one JSON object per line over a local socket.
//   request   {"id": 1, "method": "lookup", "params": {...}}
//   response  {"id": 1, "result": {...}}   or   {"id": 1, "error": {"message": "..."}}
//   event     {"event": "import.progress", "params": {...}}
// Methods and events are listed in docs/ipc.md.
namespace lexiglance::ipc
{

	inline constexpr std::size_t max_message_size = 64U << 20U;

	[[nodiscard]] std::string request( std::int64_t id, std::string_view method, std::string_view params_json = "{}" );
	[[nodiscard]] std::string response( std::int64_t id, std::string_view result_json );
	[[nodiscard]] std::string errorResponse( std::int64_t id, std::string_view message );
	[[nodiscard]] std::string event( std::string_view name, std::string_view params_json );

	// Reassembles lines from a byte stream.
	class LineBuffer
	{
	public:
		void append( std::string_view data )
		{
			buffer_.append( data );
		}

		[[nodiscard]] std::optional<std::string> next();

		[[nodiscard]] bool overflowed() const noexcept
		{
			return buffer_.size() > max_message_size;
		}

	private:
		std::string buffer_;
		std::size_t scanned_ = 0;
	};

} // namespace lexiglance::ipc

#endif // LEXIGLANCE_IPC_PROTOCOL_H
