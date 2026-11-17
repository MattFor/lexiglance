#ifndef LEXIGLANCE_DAEMON_IPCSERVER_H
#define LEXIGLANCE_DAEMON_IPCSERVER_H

#include <lexiglance/core/Error.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/ipc/Protocol.h>
#include <lexiglance/ipc/Socket.h>

#include <atomic>
#include <cstdint>
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
	// for long; those that wait (for the network, a model, the desktop) are registered with onSlow() and run on workers.
	// A client's requests are answered in the order they came, whichever kind they are.
	class IpcServer
	{
	public:
		using Handler = std::function<Result<std::string>( const json::Value& params )>;

		// Events only the clients that asked for them get (`events.subscribe`, answered by the server itself): developer
		// mode's pipeline trace and log lines.
		enum class Topic : std::uint8_t
		{
			Trace = 1,
			Log   = 2,
		};

		IpcServer();
		~IpcServer();

		IpcServer( const IpcServer& )            = delete;
		IpcServer& operator=( const IpcServer& ) = delete;
		IpcServer( IpcServer&& )                 = delete;
		IpcServer& operator=( IpcServer&& )      = delete;

		// Handlers must be registered before start().
		void on( std::string method, Handler handler );
		// A handler that may take seconds: it runs on a worker thread, so the other clients are served meanwhile, and the
		// requests its client sends after it wait for its answer. Thread-safe itself, then.
		void onSlow( std::string method, Handler handler );

		Result<> start( const std::string& endpoint );
		void     stop();

		// Thread-safe: sends an event to every connected client.
		void broadcast( std::string_view name, std::string_view params_json );

		// Thread-safe: whether a client subscribed to `topic`; nothing need be made for it otherwise.
		[[nodiscard]] bool listening( Topic topic ) const noexcept
		{
			return ( topics_.load( std::memory_order_relaxed ) & static_cast<unsigned>( topic ) ) != 0;
		}

		// Thread-safe: sends an event to the clients subscribed to `topic`.
		void publish( Topic topic, std::string_view name, std::string_view params_json );

		// Before start(): what subscribing to a topic takes (developer mode). Checked again as their events go out, so
		// they stop once it no longer holds.
		void guardTopics( std::function<Result<>()> guard );

	private:
#ifdef _WIN32
		// An overlapped operation on a pipe instance (a client's read, or waiting for a client) and its event.
		struct Pending;
#endif

		struct Route
		{
			Handler handler;
			bool    slow = false;
		};

		struct Client
		{
			ipc::Socket     socket;
			ipc::LineBuffer buffer;
			std::uint64_t   id = 0;
			// A slow request of this client's is being worked on: the ones after it wait in `buffer`.
			bool busy = false;
			// The topics it subscribed to (Topic bits).
			unsigned topics = 0;
#ifdef _WIN32
			std::unique_ptr<Pending> read;
#endif
		};

		// A message for one client (a slow request's answer), or for every client (0: an event; with a topic, only to
		// those subscribed to it).
		struct Outgoing
		{
			std::uint64_t client = 0;
			unsigned      topic  = 0;
			std::string   message;
		};

		void loop( const std::stop_token& stop );
		// Answers the client's requests waiting in its buffer, up to a slow one.
		void serve( Client& client );
		void dispatch( Client& client, std::string&& line );
		void deliver();
		void wake() const;
		// Sets this client's topics from `events.subscribe`'s list of names.
		[[nodiscard]] Result<> subscribe( Client& client, const json::Value& names );
		// The topics anyone subscribed to, after clients came, went or subscribed.
		void recount();

		std::string endpoint_;
		ipc::Socket server_;
#ifdef _WIN32
		void* wake_event_ = nullptr;
#else
		int wake_fd_ = -1;
		// The same descriptor, or on macOS the other end of the pipe.
		int wake_write_fd_ = -1;
#endif
		std::unordered_map<std::string, Route> handlers_;
		std::vector<Client>                    clients_;
		std::uint64_t                          next_client_ = 1;
		std::atomic<unsigned>                  topics_{ 0 };
		std::function<Result<>()>              topic_guard_;
		std::mutex                             outbox_mutex_;
		std::vector<Outgoing>                  outbox_;
		std::unique_ptr<ThreadPool>            workers_;
		std::jthread                           thread_;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_IPCSERVER_H
