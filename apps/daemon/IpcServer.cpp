#include "IpcServer.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Thread.h>

#include <array>
#include <filesystem>
#include <ranges>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <poll.h>
	#include <sys/eventfd.h>
	#include <unistd.h>
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
		// A client that stops reading cannot stall the server for longer.
		constexpr auto write_timeout = std::chrono::seconds( 5 );

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

	// Out of line: clients hold Windows' pending operations, complete only here.
	IpcServer::IpcServer() = default;

	IpcServer::~IpcServer()
	{
		stop();
	}

	void IpcServer::on( std::string method, Handler handler )
	{
		handlers_.emplace( std::move( method ), std::move( handler ) );
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
#else
		wake_fd_ = ::eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
		if ( wake_fd_ < 0 )
		{
			return fail( "eventfd failed" );
		}
#endif
		thread_ = std::jthread( [this]( const std::stop_token& stop ) {
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
		if ( server_.valid() )
		{
			server_.close();
#ifndef _WIN32
			std::error_code ec;
			std::filesystem::remove( endpoint_, ec );
#endif
		}
#ifdef _WIN32
		if ( wake_event_ != nullptr )
		{
			CloseHandle( wake_event_ );
			wake_event_ = nullptr;
		}
#else
		if ( wake_fd_ >= 0 )
		{
			::close( wake_fd_ );
			wake_fd_ = -1;
		}
#endif
	}

	void IpcServer::broadcast( std::string_view name, std::string_view params_json )
	{
		{
			const std::scoped_lock lock( outbox_mutex_ );
			outbox_.push_back( ipc::event( name, params_json ) );
		}
		wake();
	}

	void IpcServer::wake() const
	{
#ifdef _WIN32
		if ( wake_event_ != nullptr )
		{
			SetEvent( wake_event_ );
		}
#else
		if ( wake_fd_ >= 0 )
		{
			const std::uint64_t one = 1;
			( void )::write( wake_fd_, &one, sizeof( one ) );
		}
#endif
	}

	// Sends what broadcast() queued to every client.
	void IpcServer::deliver()
	{
		std::vector<std::string> messages;
		{
			const std::scoped_lock lock( outbox_mutex_ );
			messages.swap( outbox_ );
		}
		for ( const Client& client : clients_ )
		{
			for ( const auto& message : messages )
			{
				( void )client.socket.writeAll( message );
			}
		}
	}

	void IpcServer::dispatch( Client& client, std::string&& line )
	{
		auto message = json::Document::parse( std::move( line ) );
		if ( !message )
		{
			( void )client.socket.writeAll( ipc::errorResponse( -1, "malformed request: " + message.error().message ) );
			return;
		}

		const json::Value& root   = message->root();
		const auto         id     = root["id"].asInt( -1 );
		const auto         method = std::string( root["method"].asString() );
		const auto         it     = handlers_.find( method );
		if ( it == handlers_.end() )
		{
			( void )client.socket.writeAll( ipc::errorResponse( id, "unknown method: " + method ) );
			return;
		}

		Result<std::string> result = fail( "internal error" );
		try
		{
			result = it->second( root["params"] );
		}
		catch ( const std::exception& e )
		{
			result = fail( "{} failed: {}", method, e.what() );
		}
		( void )client.socket.writeAll( result ? ipc::response( id, *result ) : ipc::errorResponse( id, result.error().message ) );
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
			if ( ConnectNamedPipe( handleOf( server_ ), listening.restart() ) == FALSE )
			{
				const DWORD error = GetLastError();
				if ( error == ERROR_IO_PENDING )
				{
					listening.busy = true;
					return;
				}
				if ( error != ERROR_PIPE_CONNECTED )
				{
					log::warn( "ipc: cannot wait for clients (error {})", error );
					return;
				}
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
				Client client{ .socket = std::move( server_ ), .buffer = {}, .read = std::make_unique<Pending>() };
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
			while ( auto line = client.buffer.next() )
			{
				dispatch( client, std::move( *line ) );
			}
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
				std::uint64_t value = 0;
				( void )::read( wake_fd_, &value, sizeof( value ) );
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
				while ( auto line = client.buffer.next() )
				{
					dispatch( client, std::move( *line ) );
				}
			}
			for ( const std::size_t index : std::views::reverse( closed ) )
			{
				clients_.erase( clients_.begin() + static_cast<std::ptrdiff_t>( index ) );
			}

			if ( ( fds[0].revents & POLLIN ) != 0 )
			{
				if ( auto socket = ipc::acceptFrom( server_ ) )
				{
					clients_.push_back( { .socket = std::move( *socket ) } );
				}
			}
		}
	}

#endif

} // namespace lexiglance::daemon
