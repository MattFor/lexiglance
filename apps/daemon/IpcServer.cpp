#include "IpcServer.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Thread.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <ranges>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <fcntl.h>
	#include <poll.h>
	#include <sys/socket.h>
	#include <unistd.h>
	#ifndef __APPLE__
		#include <sys/eventfd.h>
	#endif
#endif

namespace lexiglance::daemon
{

#ifdef _WIN32

	namespace
	{

		HANDLE handleOf( const ipc::Socket& socket ) noexcept
		{
			return reinterpret_cast<HANDLE>( static_cast<std::intptr_t>( socket.fd() ) );
		}

		// WaitForMultipleObjects takes 64 handles: the wake event, the waiting instance and the clients.
		constexpr std::size_t max_clients = MAXIMUM_WAIT_OBJECTS - 2;

	} // namespace

	struct IpcServer::Pending
	{
		Pending() :
			event( CreateEventW( nullptr, TRUE, FALSE, nullptr ) )
		{
		}

		~Pending()
		{
			if ( event != nullptr )
			{
				CloseHandle( event );
			}
		}

		Pending( const Pending& )            = delete;
		Pending& operator=( const Pending& ) = delete;
		Pending( Pending&& )                 = delete;
		Pending& operator=( Pending&& )      = delete;

		OVERLAPPED* restart() noexcept
		{
			ResetEvent( event );
			overlapped        = {};
			overlapped.hEvent = event;
			return &overlapped;
		}

		// Ends an operation still running, before its buffer and OVERLAPPED go away.
		void cancel( HANDLE pipe ) noexcept
		{
			if ( busy )
			{
				CancelIoEx( pipe, &overlapped );
				DWORD done = 0;
				GetOverlappedResult( pipe, &overlapped, &done, TRUE );
				busy = false;
			}
		}

		OVERLAPPED                      overlapped{};
		HANDLE                          event;
		bool                            busy = false;
		std::array<char, 64UL * 1024UL> data{};
	};

#endif

	namespace
	{

		// A client that stops reading cannot stall the server for longer.
		constexpr auto write_timeout = std::chrono::seconds( 5 );

		// A client that cannot be written to is hung up on; the loop's read path then drops it (not here: a handler may be
		// running for it).
		bool sendTo( const ipc::Socket& socket, std::string_view message )
		{
			if ( socket.writeAll( message ) )
			{
				return true;
			}
#ifdef _WIN32
			// Not CloseHandle: the client's pending read still uses the handle.
			DisconnectNamedPipe( handleOf( socket ) );
#else
			::shutdown( socket.fd(), SHUT_RDWR );
#endif
			return false;
		}

		// The handler's answer to request `id`, as the line that goes to the client.
		std::string answer( const IpcServer::Handler& handler, std::string_view method, const json::Value& params, std::int64_t id )
		{
			Result<std::string> result = fail( "internal error" );
			try
			{
				result = handler( params );
			}
			catch ( const std::exception& e )
			{
				result = fail( "{} failed: {}", method, e.what() );
			}
			return result ? ipc::response( id, *result ) : ipc::errorResponse( id, result.error().message );
		}

		// Slow requests worked on at once; more wait for a free worker, while quick ones never do.
		constexpr unsigned slow_workers = 4;

	} // namespace

	// Out of line: clients hold Windows' pending operations, complete only here.
	IpcServer::IpcServer() = default;

	IpcServer::~IpcServer()
	{
		stop();
	}

	void IpcServer::on( std::string method, Handler handler )
	{
		handlers_.emplace( std::move( method ), Route{ .handler = std::move( handler ) } );
	}

	void IpcServer::onSlow( std::string method, Handler handler )
	{
		handlers_.emplace( std::move( method ), Route{ .handler = std::move( handler ), .slow = true } );
	}

	Result<> IpcServer::start( const std::string& endpoint )
	{
		auto server = ipc::listenOn( endpoint );
		if ( !server )
		{
			return std::unexpected( server.error() );
		}
		server_   = std::move( *server );
		endpoint_ = endpoint;
#ifdef _WIN32
		wake_event_ = CreateEventW( nullptr, FALSE, FALSE, nullptr );
		if ( wake_event_ == nullptr )
		{
			return fail( "CreateEvent failed (error {})", GetLastError() );
		}
#elifdef __APPLE__
		// macOS has no eventfd: a pipe, read at one end and written at the other.
		std::array<int, 2> ends{ -1, -1 };
		if ( ::pipe( ends.data() ) != 0 )
		{
			return fail( "pipe failed" );
		}
		for ( const int end : ends )
		{
			( void )::fcntl( end, F_SETFD, FD_CLOEXEC );
			( void )::fcntl( end, F_SETFL, O_NONBLOCK );
		}
		wake_fd_       = ends[0];
		wake_write_fd_ = ends[1];
#else
		wake_fd_ = ::eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
		if ( wake_fd_ < 0 )
		{
			return fail( "eventfd failed" );
		}
		wake_write_fd_ = wake_fd_;
#endif
		workers_ = std::make_unique<ThreadPool>( slow_workers, false, "lg-ipc-slow" );
		thread_  = std::jthread( [this]( const std::stop_token& stop ) {
			thread::setName( "lg-ipc" );
			loop( stop );
		} );
		return {};
	}

	void IpcServer::stop()
	{
		if ( thread_.joinable() )
		{
			thread_.request_stop();
			wake();
			thread_.join();
		}
		clients_.clear();
		recount();
		if ( server_.valid() )
		{
			server_.close();
#ifndef _WIN32
			std::error_code ec;
			std::filesystem::remove( endpoint_, ec );
#endif
		}
		// Slow requests still being worked on finish here; they wake the loop when done, so before its wake-up goes.
		workers_.reset();
#ifdef _WIN32
		if ( wake_event_ != nullptr )
		{
			CloseHandle( wake_event_ );
			wake_event_ = nullptr;
		}
#else
		if ( wake_write_fd_ != wake_fd_ )
		{
			::close( wake_write_fd_ );
		}
		if ( wake_fd_ >= 0 )
		{
			::close( wake_fd_ );
		}
		wake_fd_       = -1;
		wake_write_fd_ = -1;
#endif
	}

	void IpcServer::broadcast( std::string_view name, std::string_view params_json )
	{
		{
			const std::scoped_lock lock( outbox_mutex_ );
			outbox_.push_back( { .client = 0, .message = ipc::event( name, params_json ) } );
		}
		wake();
	}

	void IpcServer::publish( Topic topic, std::string_view name, std::string_view params_json )
	{
		{
			const std::scoped_lock lock( outbox_mutex_ );
			outbox_.push_back( { .client = 0, .topic = static_cast<unsigned>( topic ), .message = ipc::event( name, params_json ) } );
		}
		wake();
	}

	void IpcServer::guardTopics( std::function<Result<>()> guard )
	{
		topic_guard_ = std::move( guard );
	}

	Result<> IpcServer::subscribe( Client& client, const json::Value& names )
	{
		unsigned topics = 0;
		for ( const json::Value& name : names.items() )
		{
			if ( name.asString() == "trace" )
			{
				topics |= static_cast<unsigned>( Topic::Trace );
			}
			else if ( name.asString() == "log" )
			{
				topics |= static_cast<unsigned>( Topic::Log );
			}
			else
			{
				return fail( "no such events: {}", name.asString() );
			}
		}
		if ( topics != 0 && topic_guard_ )
		{
			if ( auto allowed = topic_guard_(); !allowed )
			{
				return allowed;
			}
		}
		client.topics = topics;
		recount();
		return {};
	}

	void IpcServer::recount()
	{
		unsigned topics = 0;
		for ( const Client& client : clients_ )
		{
			topics |= client.topics;
		}
		topics_.store( topics, std::memory_order_relaxed );
	}

	void IpcServer::wake() const
	{
#ifdef _WIN32
		if ( wake_event_ != nullptr )
		{
			SetEvent( wake_event_ );
		}
#else
		if ( wake_write_fd_ >= 0 )
		{
			const std::uint64_t         one     = 1;
			[[maybe_unused]] const auto written = ::write( wake_write_fd_, &one, sizeof( one ) );
		}
#endif
	}

	// Sends what broadcast() and the slow requests queued, to each client its share in order. A client whose answer came
	// goes on with the requests that waited for it.
	void IpcServer::deliver()
	{
		std::vector<Outgoing> messages;
		{
			const std::scoped_lock lock( outbox_mutex_ );
			messages.swap( outbox_ );
		}
		// What the topics take no longer holds (developer mode turned off): they stop.
		const auto topical = []( const Outgoing& message ) { return message.topic != 0; };
		if ( topic_guard_ && std::ranges::any_of( messages, topical ) && !topic_guard_() )
		{
			for ( Client& client : clients_ )
			{
				client.topics = 0;
			}
			recount();
			std::erase_if( messages, topical );
		}
		for ( Client& client : clients_ )
		{
			for ( const auto& [to, topic, message] : messages )
			{
				const bool wanted = to == client.id || ( to == 0 && ( topic == 0 || ( client.topics & topic ) != 0 ) );
				if ( wanted && !sendTo( client.socket, message ) )
				{
					break;
				}
			}
			if ( std::ranges::contains( messages, client.id, &Outgoing::client ) )
			{
				client.busy = false;
				serve( client );
			}
		}
	}

	void IpcServer::serve( Client& client )
	{
		while ( !client.busy )
		{
			auto line = client.buffer.next();
			if ( !line )
			{
				return;
			}
			dispatch( client, std::move( *line ) );
		}
	}

	void IpcServer::dispatch( Client& client, std::string&& line )
	{
		auto message = json::Document::parse( std::move( line ) );
		if ( !message )
		{
			sendTo( client.socket, ipc::errorResponse( -1, "malformed request: " + message.error().message ) );
			return;
		}

		const json::Value& root   = message->root();
		const auto         id     = root["id"].asInt( -1 );
		auto               method = std::string( root["method"].asString() );
		if ( method == "events.subscribe" )
		{
			const auto subscribed = subscribe( client, root["params"]["events"] );
			sendTo( client.socket, subscribed ? ipc::response( id, "{}" ) : ipc::errorResponse( id, subscribed.error().message ) );
			return;
		}
		const auto it = handlers_.find( method );
		if ( it == handlers_.end() )
		{
			sendTo( client.socket, ipc::errorResponse( id, "unknown method: " + method ) );
			return;
		}
		if ( !it->second.slow )
		{
			sendTo( client.socket, answer( it->second.handler, method, root["params"], id ) );
			return;
		}

		// The answer goes out through the outbox, after the events sent before it.
		client.busy = true;
		( void )workers_->submit( [this, owner = client.id, handler = &it->second.handler, method = std::move( method ), id, request = std::move( *message )] {
			auto reply = answer( *handler, method, request.root()["params"], id );
			{
				const std::scoped_lock lock( outbox_mutex_ );
				outbox_.push_back( { .client = owner, .message = std::move( reply ) } );
			}
			wake();
		} );
	}

#ifdef _WIN32

	// Named pipes have no poll(): every client has a read pending, the newest instance waits for the next client, and
	// the thread waits for any of them or the wake event.
	void IpcServer::loop( const std::stop_token& stop )
	{
		Pending listening;
		// The waiting instance's client came before ConnectNamedPipe asked (its event is set by hand then).
		bool arrived = false;

		const auto await = [&] {
			arrived = false;
			for ( int tries = 1; ConnectNamedPipe( handleOf( server_ ), listening.restart() ) == FALSE; ++tries )
			{
				const DWORD error = GetLastError();
				if ( error == ERROR_IO_PENDING )
				{
					listening.busy = true;
					return;
				}
				if ( error == ERROR_PIPE_CONNECTED )
				{
					break;
				}
				// A client came and went before this asked (one only looking whether the daemon answers, as a daemon
				// making way for another does): the instance has to let go of it before it can wait for the next.
				// Otherwise nothing would be accepted again, and every client would wait for good.
				if ( error == ERROR_NO_DATA && tries < 100 )
				{
					DisconnectNamedPipe( handleOf( server_ ) );
					continue;
				}
				log::warn( "ipc: cannot wait for clients (error {})", error );
				return;
			}
			arrived = true;
			SetEvent( listening.event );
		};

		const auto read = [&]( Client& client ) {
			Pending& pending = *client.read;
			if ( ReadFile( handleOf( client.socket ), pending.data.data(), static_cast<DWORD>( pending.data.size() ), nullptr, pending.restart() ) == FALSE && GetLastError() != ERROR_IO_PENDING )
			{
				return false;
			}
			pending.busy = true;
			return true;
		};

		await();
		std::vector<HANDLE> events;
		while ( !stop.stop_requested() )
		{
			std::vector<std::size_t> closed;
			for ( std::size_t i = 0; i < clients_.size(); ++i )
			{
				if ( !clients_[i].read->busy && !read( clients_[i] ) )
				{
					closed.push_back( i );
				}
			}
			for ( const std::size_t index : std::views::reverse( closed ) )
			{
				clients_.erase( clients_.begin() + static_cast<std::ptrdiff_t>( index ) );
			}
			// Clients that went took their subscriptions along.
			recount();

			events.clear();
			events.push_back( wake_event_ );
			const bool accepting = ( listening.busy || arrived ) && clients_.size() < max_clients;
			if ( accepting )
			{
				events.push_back( listening.event );
			}
			const std::size_t first_client = events.size();
			for ( const Client& client : clients_ )
			{
				events.push_back( client.read->event );
			}

			const DWORD signalled = WaitForMultipleObjects( static_cast<DWORD>( events.size() ), events.data(), FALSE, INFINITE );
			if ( signalled == WAIT_FAILED || signalled >= WAIT_OBJECT_0 + events.size() )
			{
				log::warn( "ipc: waiting failed (error {})", GetLastError() );
				std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
				continue;
			}
			const std::size_t index = signalled - WAIT_OBJECT_0;
			if ( index == 0 )
			{
				deliver();
				continue;
			}

			if ( accepting && index == 1 )
			{
				DWORD      ignored   = 0;
				const bool connected = arrived || GetOverlappedResult( handleOf( server_ ), &listening.overlapped, &ignored, FALSE ) != FALSE;
				listening.busy       = false;
				if ( !connected )
				{
					// The client gave up before it was served: the instance waits for another.
					DisconnectNamedPipe( handleOf( server_ ) );
					await();
					continue;
				}
				Client client{ .socket = std::move( server_ ), .buffer = {}, .id = next_client_++, .read = std::make_unique<Pending>() };
				( void )client.socket.setTimeout( write_timeout );
				clients_.push_back( std::move( client ) );
				if ( auto next = ipc::createPipeInstance( endpoint_, false ) )
				{
					server_ = std::move( *next );
					await();
				}
				else
				{
					log::warn( "ipc: {}", next.error().message );
				}
				continue;
			}

			Client&    client   = clients_[index - first_client];
			DWORD      received = 0;
			const bool ok       = GetOverlappedResult( handleOf( client.socket ), &client.read->overlapped, &received, FALSE ) != FALSE;
			client.read->busy   = false;
			if ( !ok )
			{
				clients_.erase( clients_.begin() + static_cast<std::ptrdiff_t>( index - first_client ) );
				continue;
			}
			client.buffer.append( std::string_view( client.read->data.data(), received ) );
			if ( client.buffer.overflowed() )
			{
				clients_.erase( clients_.begin() + static_cast<std::ptrdiff_t>( index - first_client ) );
				continue;
			}
			serve( client );
		}

		for ( Client& client : clients_ )
		{
			client.read->cancel( handleOf( client.socket ) );
		}
		if ( server_.valid() )
		{
			listening.cancel( handleOf( server_ ) );
		}
	}

#else

	void IpcServer::loop( const std::stop_token& stop )
	{
		std::array<char, 64UL * 1024UL> chunk{};
		std::vector<pollfd>             fds;

		while ( !stop.stop_requested() )
		{
			// Clients that went took their subscriptions along.
			recount();
			fds.clear();
			fds.push_back( { .fd = server_.fd(), .events = POLLIN, .revents = 0 } );
			fds.push_back( { .fd = wake_fd_, .events = POLLIN, .revents = 0 } );
			for ( const Client& client : clients_ )
			{
				fds.push_back( { .fd = client.socket.fd(), .events = POLLIN, .revents = 0 } );
			}

			if ( ::poll( fds.data(), fds.size(), -1 ) < 0 )
			{
				continue;
			}

			if ( ( fds[1].revents & POLLIN ) != 0 )
			{
				std::uint64_t               value   = 0;
				[[maybe_unused]] const auto drained = ::read( wake_fd_, &value, sizeof( value ) );
				deliver();
			}

			// Clients first (indices in fds match clients_), then accept new ones.
			std::vector<std::size_t> closed;
			for ( std::size_t i = 0; i < clients_.size(); ++i )
			{
				const short revents = fds[i + 2].revents;
				if ( ( revents & ( POLLIN | POLLHUP | POLLERR ) ) == 0 )
				{
					continue;
				}
				Client&    client   = clients_[i];
				const auto received = client.socket.readSome( chunk );
				if ( !received || *received == 0 )
				{
					closed.push_back( i );
					continue;
				}
				client.buffer.append( std::string_view( chunk.data(), *received ) );
				if ( client.buffer.overflowed() )
				{
					closed.push_back( i );
					continue;
				}
				serve( client );
			}
			for ( const std::size_t index : std::views::reverse( closed ) )
			{
				clients_.erase( clients_.begin() + static_cast<std::ptrdiff_t>( index ) );
			}

			if ( ( fds[0].revents & POLLIN ) != 0 )
			{
				if ( auto socket = ipc::acceptFrom( server_ ) )
				{
					( void )socket->setTimeout( write_timeout );
					clients_.push_back( { .socket = std::move( *socket ), .id = next_client_++ } );
				}
			}
		}
	}

#endif

} // namespace lexiglance::daemon
