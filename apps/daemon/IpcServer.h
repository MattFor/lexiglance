#ifndef LEXIGLANCE_DAEMON_IPCSERVER_H
#define LEXIGLANCE_DAEMON_IPCSERVER_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/ipc/Protocol.h>
#include <lexiglance/ipc/Socket.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lexiglance::daemon
{

	// Serves the local socket (a named pipe on Windows) on its own thread. Handlers run on that thread and must not block
	// for long; long running work is handed to worker threads which report back through broadcast().
	class IpcServer
	{
	public:
		using Handler = std::function<Result<std::string>( const json::Value& params )>;

		IpcServer();
		~IpcServer();

		IpcServer( const IpcServer& )            = delete;
		IpcServer& operator=( const IpcServer& ) = delete;
		IpcServer( IpcServer&& )                 = delete;
		IpcServer& operator=( IpcServer&& )      = delete;

		// Handlers must be registered before start().
		void on( std::string method, Handler handler );

		Result<> start( const std::string& endpoint );
		void     stop();

		// Thread-safe: sends an event to every connected client.
		void broadcast( std::string_view name, std::string_view params_json );

	private:
#ifdef _WIN32
		// An overlapped operation on a pipe instance (a client's read, or waiting for a client) and its event.
		struct Pending;
#endif

		struct Client
		{
			ipc::Socket     socket;
			ipc::LineBuffer buffer;
#ifdef _WIN32
			std::unique_ptr<Pending> read;
#endif
		};

		void loop( const std::stop_token& stop );
		void dispatch( Client& client, std::string&& line );
		void deliver();
		void wake() const;

		std::string endpoint_;
		ipc::Socket server_;
#ifdef _WIN32
		void* wake_event_ = nullptr;
#else
		int wake_fd_ = -1;
#endif
		std::unordered_map<std::string, Handler> handlers_;
		std::vector<Client>                      clients_;
		std::mutex                               outbox_mutex_;
		std::vector<std::string>                 outbox_;
		std::jthread                             thread_;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_IPCSERVER_H
