#ifndef LEXIGLANCE_IPC_SOCKET_H
#define LEXIGLANCE_IPC_SOCKET_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/ipc/Protocol.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace lexiglance::ipc
{

	// Owning stream socket: a Unix domain socket, or on Windows a named pipe.
	class Socket
	{
	public:
		Socket() noexcept = default;
		explicit Socket( int fd ) noexcept :
			fd_( fd )
		{
		}
		~Socket();

		Socket( const Socket& )            = delete;
		Socket& operator=( const Socket& ) = delete;
		Socket( Socket&& other ) noexcept;
		Socket& operator=( Socket&& other ) noexcept;

		[[nodiscard]] int fd() const noexcept
		{
			return fd_;
		}

		[[nodiscard]] bool valid() const noexcept
		{
			return fd_ >= 0;
		}

		void close() noexcept;

		// Sending, receiving and connecting give up after `timeout` (0: wait for ever).
		[[nodiscard]] Result<> setTimeout( std::chrono::milliseconds timeout ) const;

		[[nodiscard]] Result<> writeAll( std::string_view data ) const;

		// Returns 0 at end of stream.
		[[nodiscard]] Result<std::size_t> readSome( std::span<char> buffer ) const;

	private:
		// A Unix socket, or on Windows a pipe handle (64-bit Windows keeps handles in 32 bits).
		int fd_ = -1;
#ifdef _WIN32
		// Pipes have no SO_RCVTIMEO: each read and write waits at most this long (0: for ever). A setting of the
		// handle, like the socket option, hence mutable.
		mutable std::chrono::milliseconds timeout_{ 0 };
#endif
	};

	// With a timeout, a daemon that no longer answers cannot block the caller (0: wait for ever).
	[[nodiscard]] Result<Socket> connectTo( const std::string& endpoint, std::chrono::milliseconds timeout = std::chrono::milliseconds( 0 ) );

	// Refuses to take over an endpoint another live process is serving; stale sockets are replaced.
	[[nodiscard]] Result<Socket> listenOn( const std::string& endpoint );

	[[nodiscard]] Result<Socket> acceptFrom( const Socket& server );

#ifdef _WIN32
	// A named pipe instance for the next client, which the daemon's server loop connects (listenOn() makes the first).
	// Only this user can open it; `first` refuses a name another process already serves.
	[[nodiscard]] Result<Socket> createPipeInstance( const std::string& endpoint, bool first );
#endif

	// Blocking request/response client, used by scripts and tests.
	class Client
	{
	public:
		using EventHandler = std::function<void( std::string_view name, const json::Value& params )>;

		// `timeout` bounds connecting and every read and write of a call (0: wait for ever).
		[[nodiscard]] static Result<Client> connect( const std::string& endpoint, std::chrono::milliseconds timeout = std::chrono::milliseconds( 0 ) );

		// Returns the whole response document; the payload is root()["result"].
		[[nodiscard]] Result<json::Document> call( std::string_view method, std::string_view params_json = "{}", const EventHandler& on_event = {} );

	private:
		Socket       socket_;
		LineBuffer   buffer_;
		std::int64_t next_id_ = 1;
	};

} // namespace lexiglance::ipc

#endif // LEXIGLANCE_IPC_SOCKET_H
