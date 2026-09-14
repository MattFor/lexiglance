#include <lexiglance/ipc/Socket.h>

#include <lexiglance/core/Process.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	// After windows.h, which it needs.
	#include <sddl.h>

	#include <vector>
#else
	#include <sys/socket.h>
	#include <sys/stat.h>
	#include <sys/time.h>
	#include <sys/un.h>
	#include <unistd.h>
#endif

namespace lexiglance::ipc
{

#ifndef _WIN32
	namespace
	{

		std::string errorText( int err )
		{
			return std::generic_category().message( err );
		}

	} // namespace
#endif

	Socket::~Socket()
	{
		close();
	}

	Socket::Socket( Socket&& other ) noexcept :
		fd_( std::exchange( other.fd_, -1 ) )
#ifdef _WIN32
		,
		timeout_( other.timeout_ )
#endif
	{
	}

	Socket& Socket::operator=( Socket&& other ) noexcept
	{
		if ( this != &other )
		{
			close();
			fd_ = std::exchange( other.fd_, -1 );
#ifdef _WIN32
			timeout_ = other.timeout_;
#endif
		}
		return *this;
	}

#ifdef _WIN32

	namespace
	{

		HANDLE handleOf( int fd ) noexcept
		{
			return reinterpret_cast<HANDLE>( static_cast<std::intptr_t>( fd ) );
		}

		// 64-bit Windows keeps handles in their low 32 bits (sign-extended), so a handle fits the descriptor.
		int descriptorOf( HANDLE handle ) noexcept
		{
			return static_cast<int>( reinterpret_cast<std::intptr_t>( handle ) );
		}

		std::wstring wide( std::string_view text )
		{
			if ( text.empty() )
			{
				return {};
			}
			const int    length = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), nullptr, 0 );
			std::wstring out( static_cast<std::size_t>( std::max( 0, length ) ), L'\0' );
			MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast<int>( text.size() ), out.data(), length );
			return out;
		}

		DWORD waitTime( std::chrono::milliseconds timeout ) noexcept
		{
			return timeout > std::chrono::milliseconds::zero() ? static_cast<DWORD>( std::min<long long>( timeout.count(), 0x7FFFFFFF ) ) : INFINITE;
		}

		// An overlapped read or write that gives up after `timeout`: the bytes moved, 0 at the end of the stream.
		Result<std::size_t> transfer( HANDLE pipe, bool write, void* data, std::size_t size, std::chrono::milliseconds timeout )
		{
			OVERLAPPED overlapped{};
			overlapped.hEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );
			if ( overlapped.hEvent == nullptr )
			{
				return fail( "CreateEvent failed (error {})", GetLastError() );
			}
			const auto length  = static_cast<DWORD>( std::min<std::size_t>( size, std::size_t{ 1 } << 20U ) );
			const BOOL started = write ? WriteFile( pipe, data, length, nullptr, &overlapped ) : ReadFile( pipe, data, length, nullptr, &overlapped );
			DWORD      error   = started != FALSE ? ERROR_SUCCESS : GetLastError();
			DWORD      done    = 0;
			if ( started != FALSE || error == ERROR_IO_PENDING )
			{
				if ( WaitForSingleObject( overlapped.hEvent, waitTime( timeout ) ) != WAIT_OBJECT_0 )
				{
					// The cancelled operation must end before its buffer and OVERLAPPED go away.
					CancelIoEx( pipe, &overlapped );
					GetOverlappedResult( pipe, &overlapped, &done, TRUE );
					CloseHandle( overlapped.hEvent );
					return fail( "{}", write ? "timed out sending to the daemon" : "timed out: the daemon did not answer" );
				}
				error = GetOverlappedResult( pipe, &overlapped, &done, FALSE ) != FALSE ? ERROR_SUCCESS : GetLastError();
			}
			CloseHandle( overlapped.hEvent );
			if ( error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA )
			{
				return std::size_t{ 0 };
			}
			if ( error != ERROR_SUCCESS && error != ERROR_MORE_DATA )
			{
				return fail( "{} failed (error {})", write ? "WriteFile" : "ReadFile", error );
			}
			return static_cast<std::size_t>( done );
		}

	} // namespace

	void Socket::close() noexcept
	{
		if ( fd_ != -1 )
		{
			CloseHandle( handleOf( fd_ ) );
			fd_ = -1;
		}
	}

	Result<> Socket::setTimeout( std::chrono::milliseconds timeout ) const
	{
		timeout_ = timeout;
		return {};
	}

	Result<> Socket::writeAll( std::string_view data ) const
	{
		while ( !data.empty() )
		{
			auto written = transfer( handleOf( fd_ ), true, const_cast<char*>( data.data() ), data.size(), timeout_ );
			if ( !written )
			{
				return std::unexpected( written.error() );
			}
			if ( *written == 0 )
			{
				return fail( "the daemon closed the connection" );
			}
			data.remove_prefix( *written );
		}
		return {};
	}

	Result<std::size_t> Socket::readSome( std::span<char> buffer ) const
	{
		return transfer( handleOf( fd_ ), false, buffer.data(), buffer.size(), timeout_ );
	}

	Result<Socket> connectTo( const std::string& endpoint, std::chrono::milliseconds timeout )
	{
		const std::wstring name  = wide( endpoint );
		const auto         until = std::chrono::steady_clock::now() + timeout;
		while ( true )
		{
			// Identification only: whoever serves the pipe learns who connects, but cannot act as the user.
			HANDLE pipe = CreateFileW( name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr );
			if ( pipe != INVALID_HANDLE_VALUE )
			{
				Socket socket( descriptorOf( pipe ) );
				// The daemon runs as this user; a pipe of that name another account made first is not it.
				ULONG server = 0;
				if ( GetNamedPipeServerProcessId( pipe, &server ) == FALSE || !process::sameUser( static_cast<int>( server ) ) )
				{
					return fail( "{} is not served by this user's daemon", endpoint );
				}
				( void )socket.setTimeout( timeout );
				return socket;
			}
			const DWORD error = GetLastError();
			if ( error != ERROR_PIPE_BUSY )
			{
				return fail( "cannot connect to {} (error {})", endpoint, error );
			}
			// Every instance is busy with another client: wait for one to come free.
			DWORD wait = NMPWAIT_WAIT_FOREVER;
			if ( timeout > std::chrono::milliseconds::zero() )
			{
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>( until - std::chrono::steady_clock::now() );
				if ( left <= std::chrono::milliseconds::zero() )
				{
					return fail( "cannot connect to {}: timed out", endpoint );
				}
				wait = waitTime( left );
			}
			if ( WaitNamedPipeW( name.c_str(), wait ) == FALSE && GetLastError() == ERROR_SEM_TIMEOUT )
			{
				return fail( "cannot connect to {}: timed out", endpoint );
			}
		}
	}

	namespace
	{

		// Only this user (and the system) may open the pipe: "D:P(A;;GA;;;SY)(A;;GA;;;<user>)". A pipe's default
		// security would let every account read it.
		std::wstring pipeSecurity()
		{
			HANDLE token = nullptr;
			if ( OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ) == FALSE )
			{
				return {};
			}
			DWORD size = 0;
			GetTokenInformation( token, TokenUser, nullptr, 0, &size );
			std::vector<unsigned char> buffer( size );
			std::wstring               descriptor;
			LPWSTR                     sid = nullptr;
			if ( size > 0 && GetTokenInformation( token, TokenUser, buffer.data(), size, &size ) != FALSE && ConvertSidToStringSidW( reinterpret_cast<const TOKEN_USER*>( buffer.data() )->User.Sid, &sid ) != FALSE )
			{
				descriptor = std::wstring( L"D:P(A;;GA;;;SY)(A;;GA;;;" ) + sid + L")";
				LocalFree( sid );
			}
			CloseHandle( token );
			return descriptor;
		}

	} // namespace

	Result<Socket> createPipeInstance( const std::string& endpoint, bool first )
	{
		const std::wstring   descriptor = pipeSecurity();
		PSECURITY_DESCRIPTOR security   = nullptr;
		if ( descriptor.empty() || ConvertStringSecurityDescriptorToSecurityDescriptorW( descriptor.c_str(), SDDL_REVISION_1, &security, nullptr ) == FALSE )
		{
			return fail( "cannot restrict {} to this user (error {})", endpoint, GetLastError() );
		}
		SECURITY_ATTRIBUTES attributes{ .nLength = sizeof( SECURITY_ATTRIBUTES ), .lpSecurityDescriptor = security, .bInheritHandle = FALSE };
		// Only the first instance may create the name: a pipe of that name another process serves is not taken over.
		const DWORD mode  = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | ( first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0U );
		HANDLE      pipe  = CreateNamedPipeW( wide( endpoint ).c_str(), mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 64U * 1024U, 64U * 1024U, 0, &attributes );
		const DWORD error = GetLastError();
		LocalFree( security );
		if ( pipe == INVALID_HANDLE_VALUE )
		{
			if ( first && ( error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY ) )
			{
				return fail( "another instance is already listening on {}", endpoint );
			}
			return fail( "cannot create {} (error {})", endpoint, error );
		}
		return Socket( descriptorOf( pipe ) );
	}

	Result<Socket> listenOn( const std::string& endpoint )
	{
		return createPipeInstance( endpoint, true );
	}

	// Pipe instances are connected by the daemon's server loop, which waits for clients and their messages together.
	Result<Socket> acceptFrom( const Socket& )
	{
		return fail( "named pipe instances are connected by the server loop" );
	}

#else

	namespace
	{

		Result<sockaddr_un> address( const std::string& endpoint )
		{
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			if ( endpoint.size() >= sizeof( addr.sun_path ) )
			{
				return fail( "socket path too long: {}", endpoint );
			}
			std::memcpy( static_cast<char*>( addr.sun_path ), endpoint.c_str(), endpoint.size() + 1 );
			return addr;
		}

	} // namespace

	void Socket::close() noexcept
	{
		if ( fd_ >= 0 )
		{
			::close( fd_ );
			fd_ = -1;
		}
	}

	Result<> Socket::setTimeout( std::chrono::milliseconds timeout ) const
	{
		const auto    seconds = std::chrono::duration_cast<std::chrono::seconds>( timeout );
		const auto    micros  = std::chrono::duration_cast<std::chrono::microseconds>( timeout - seconds );
		const timeval value{ .tv_sec = static_cast<time_t>( seconds.count() ), .tv_usec = static_cast<suseconds_t>( micros.count() ) };
		if ( ::setsockopt( fd_, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof( value ) ) != 0 || ::setsockopt( fd_, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof( value ) ) != 0 )
		{
			return fail( "setsockopt: {}", errorText( errno ) );
		}
		return {};
	}

	Result<> Socket::writeAll( std::string_view data ) const
	{
		while ( !data.empty() )
		{
			const auto written = ::send( fd_, data.data(), data.size(), MSG_NOSIGNAL );
			if ( written < 0 )
			{
				if ( errno == EINTR )
				{
					continue;
				}
				if ( errno == EAGAIN || errno == EWOULDBLOCK )
				{
					return fail( "timed out sending to the daemon" );
				}
				return fail( "send: {}", errorText( errno ) );
			}
			data.remove_prefix( static_cast<std::size_t>( written ) );
		}
		return {};
	}

	Result<std::size_t> Socket::readSome( std::span<char> buffer ) const
	{
		while ( true )
		{
			const auto received = ::recv( fd_, buffer.data(), buffer.size(), 0 );
			if ( received >= 0 )
			{
				return static_cast<std::size_t>( received );
			}
			if ( errno == EAGAIN || errno == EWOULDBLOCK )
			{
				return fail( "timed out: the daemon did not answer" );
			}
			if ( errno != EINTR )
			{
				return fail( "recv: {}", errorText( errno ) );
			}
		}
	}

	Result<Socket> connectTo( const std::string& endpoint, std::chrono::milliseconds timeout )
	{
		const auto addr = address( endpoint );
		if ( !addr )
		{
			return std::unexpected( addr.error() );
		}

		Socket socket( ::socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 ) );
		if ( !socket.valid() )
		{
			return fail( "socket: {}", errorText( errno ) );
		}
		// Connecting to a listener whose backlog is full (it no longer accepts) waits for the send timeout.
		if ( timeout > std::chrono::milliseconds::zero() )
		{
			if ( auto set = socket.setTimeout( timeout ); !set )
			{
				return std::unexpected( set.error() );
			}
		}
		if ( ::connect( socket.fd(), reinterpret_cast<const sockaddr*>( &*addr ), sizeof( sockaddr_un ) ) != 0 )
		{
			if ( errno == EAGAIN || errno == EINPROGRESS )
			{
				return fail( "cannot connect to {}: timed out", endpoint );
			}
			return fail( "cannot connect to {}: {}", endpoint, errorText( errno ) );
		}
		return socket;
	}

	Result<Socket> listenOn( const std::string& endpoint )
	{
		const auto addr = address( endpoint );
		if ( !addr )
		{
			return std::unexpected( addr.error() );
		}

		std::error_code ec;
		if ( std::filesystem::exists( endpoint, ec ) )
		{
			// A listener that is alive but stuck times out instead of blocking; it still counts as running.
			if ( auto other = connectTo( endpoint, std::chrono::milliseconds( 500 ) ); other || other.error().message.ends_with( "timed out" ) )
			{
				return fail( "another instance is already listening on {}", endpoint );
			}
			std::filesystem::remove( endpoint, ec );
		}

		Socket socket( ::socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 ) );
		if ( !socket.valid() )
		{
			return fail( "socket: {}", errorText( errno ) );
		}

		// Created owner-only from the start; the directory is 0700 as well.
		const mode_t previous = ::umask( 0077 );
		const int    bound    = ::bind( socket.fd(), reinterpret_cast<const sockaddr*>( &*addr ), sizeof( sockaddr_un ) );
		const int    err      = errno;
		::umask( previous );
		if ( bound != 0 )
		{
			return fail( "cannot bind {}: {}", endpoint, errorText( err ) );
		}
		if ( ::listen( socket.fd(), 16 ) != 0 )
		{
			return fail( "listen: {}", errorText( errno ) );
		}
		return socket;
	}

	Result<Socket> acceptFrom( const Socket& server )
	{
		const int fd = ::accept4( server.fd(), nullptr, nullptr, SOCK_CLOEXEC );
		if ( fd < 0 )
		{
			return fail( "accept: {}", errorText( errno ) );
		}
		return Socket( fd );
	}

#endif

	Result<Client> Client::connect( const std::string& endpoint, std::chrono::milliseconds timeout )
	{
		auto socket = connectTo( endpoint, timeout );
		if ( !socket )
		{
			return std::unexpected( socket.error() );
		}
		Client client;
		client.socket_ = std::move( *socket );
		return client;
	}

	Result<json::Document> Client::call( std::string_view method, std::string_view params_json, const EventHandler& on_event )
	{
		const std::int64_t id = next_id_++;
		if ( auto sent = socket_.writeAll( request( id, method, params_json ) ); !sent )
		{
			return std::unexpected( sent.error() );
		}

		std::array<char, 64UL * 1024UL> chunk{};
		while ( true )
		{
			while ( auto line = buffer_.next() )
			{
				auto message = json::Document::parse( std::move( *line ) );
				if ( !message )
				{
					return failWith( "malformed message from daemon", message.error() );
				}
				const json::Value& root = message->root();
				if ( const auto* name = root.find( "event" ); name != nullptr )
				{
					if ( on_event )
					{
						on_event( name->asString(), root["params"] );
					}
					continue;
				}
				if ( root["id"].asInt( -1 ) != id )
				{
					continue;
				}
				if ( const auto* error = root.find( "error" ); error != nullptr )
				{
					return fail( "{}", ( *error )["message"].asString( "request failed" ) );
				}
				return std::move( *message );
			}

			const auto received = socket_.readSome( chunk );
			if ( !received )
			{
				return std::unexpected( received.error() );
			}
			if ( *received == 0 )
			{
				return fail( "daemon closed the connection" );
			}
			buffer_.append( std::string_view( chunk.data(), *received ) );
			if ( buffer_.overflowed() )
			{
				return fail( "response too large" );
			}
		}
	}

} // namespace lexiglance::ipc
