#include <lexiglance/core/Process.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	// After windows.h, which it needs.
	#include <tlhelp32.h>
#else
	#include <csignal>
	#include <fcntl.h>
	#include <sys/file.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace lexiglance::process
{

	namespace fs = std::filesystem;

	namespace
	{

		std::string readFile( const fs::path& path )
		{
			const std::ifstream in( path, std::ios::binary );
			std::ostringstream  text;
			text << in.rdbuf();
			return text.str();
		}

		template <typename T = int>
		T parseInt( std::string_view text )
		{
			T value = 0;
			while ( !text.empty() && ( text.front() == ' ' || text.front() == '\n' ) )
			{
				text.remove_prefix( 1 );
			}
			( void )std::from_chars( text.data(), text.data() + text.size(), value );
			return value;
		}

		template <typename Predicate>
		[[maybe_unused]] bool waitFor( Predicate done, std::chrono::milliseconds timeout )
		{
			const auto until = std::chrono::steady_clock::now() + timeout;
			while ( !done() )
			{
				if ( std::chrono::steady_clock::now() >= until )
				{
					return false;
				}
				std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
			}
			return true;
		}

	} // namespace

	InstanceLock::InstanceLock( fs::path file ) :
		file_( std::move( file ) )
	{
		retry();
	}

	InstanceLock::~InstanceLock()
	{
		release();
	}

	InstanceLock::InstanceLock( InstanceLock&& other ) noexcept :
		file_( std::move( other.file_ ) ),
		fd_( std::exchange( other.fd_, -1 ) ),
		held_( std::exchange( other.held_, false ) )
	{
	}

	InstanceLock& InstanceLock::operator=( InstanceLock&& other ) noexcept
	{
		if ( this != &other )
		{
			release();
			file_ = std::move( other.file_ );
			fd_   = std::exchange( other.fd_, -1 );
			held_ = std::exchange( other.held_, false );
		}
		return *this;
	}

	int InstanceLock::holder() const
	{
		return parseInt( readFile( file_ ) );
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

		class Handle
		{
		public:
			explicit Handle( HANDLE handle ) noexcept :
				handle_( handle == INVALID_HANDLE_VALUE ? nullptr : handle )
			{
			}

			~Handle()
			{
				if ( handle_ != nullptr )
				{
					CloseHandle( handle_ );
				}
			}

			Handle( const Handle& )            = delete;
			Handle& operator=( const Handle& ) = delete;
			Handle( Handle&& )                 = delete;
			Handle& operator=( Handle&& )      = delete;

			[[nodiscard]] HANDLE get() const noexcept
			{
				return handle_;
			}

			explicit operator bool() const noexcept
			{
				return handle_ != nullptr;
			}

		private:
			HANDLE handle_;
		};

		// The account a process runs as: its token's user SID (empty when that cannot be told).
		std::vector<unsigned char> userOf( HANDLE process )
		{
			HANDLE token = nullptr;
			if ( OpenProcessToken( process, TOKEN_QUERY, &token ) == FALSE )
			{
				return {};
			}
			const Handle owner( token );
			DWORD        size = 0;
			GetTokenInformation( token, TokenUser, nullptr, 0, &size );
			std::vector<unsigned char> buffer( size );
			if ( size == 0 || GetTokenInformation( token, TokenUser, buffer.data(), size, &size ) == FALSE )
			{
				return {};
			}
			const auto* user = reinterpret_cast<const TOKEN_USER*>( buffer.data() );
			const auto* sid  = static_cast<const unsigned char*>( user->User.Sid );
			return { sid, sid + GetLengthSid( user->User.Sid ) };
		}

		// Whether an executable's path or file name is `name` (without ".exe", in any case).
		bool matches( std::wstring_view image, std::string_view name )
		{
			if ( const auto slash = image.find_last_of( L"\\/" ); slash != std::wstring_view::npos )
			{
				image.remove_prefix( slash + 1 );
			}
			const auto lower = []( wchar_t c ) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>( c - L'A' + L'a' ) : c; };
			if ( image.size() > 4 && lower( image[image.size() - 4] ) == L'.' && lower( image[image.size() - 3] ) == L'e' && lower( image[image.size() - 2] ) == L'x' &&
			     lower( image[image.size() - 1] ) == L'e' )
			{
				image.remove_suffix( 4 );
			}
			return image.size() == name.size() && std::ranges::equal( image, name, {}, lower, [&]( char c ) { return lower( static_cast<wchar_t>( static_cast<unsigned char>( c ) ) ); } );
		}

	} // namespace

	void InstanceLock::release() noexcept
	{
		if ( fd_ != -1 )
		{
			if ( held_ )
			{
				OVERLAPPED at{};
				at.OffsetHigh = 1;
				UnlockFileEx( handleOf( fd_ ), 0, 1, 0, &at );
			}
			CloseHandle( handleOf( fd_ ) );
			fd_ = -1;
		}
		held_ = false;
	}

	bool InstanceLock::retry()
	{
		if ( held_ )
		{
			return true;
		}
		if ( fd_ == -1 )
		{
			// Not inherited (no security attributes): a program the daemon starts never keeps the lock.
			HANDLE file = CreateFileW( file_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
			if ( file == INVALID_HANDLE_VALUE )
			{
				return false;
			}
			fd_ = descriptorOf( file );
		}
		// A byte far past the pid is locked, so the pid itself stays readable for whoever asks who holds the lock.
		OVERLAPPED at{};
		at.OffsetHigh = 1;
		if ( LockFileEx( handleOf( fd_ ), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at ) == FALSE )
		{
			return false;
		}
		held_                   = true;
		const std::string   pid = std::to_string( GetCurrentProcessId() ) + "\n";
		const LARGE_INTEGER start{};
		DWORD               written = 0;
		if ( SetFilePointerEx( handleOf( fd_ ), start, nullptr, FILE_BEGIN ) != FALSE && SetEndOfFile( handleOf( fd_ ) ) != FALSE )
		{
			WriteFile( handleOf( fd_ ), pid.data(), static_cast<DWORD>( pid.size() ), &written, nullptr );
		}
		return true;
	}

	bool sameUser( int pid )
	{
		if ( pid <= 0 )
		{
			return false;
		}
		const Handle process( OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>( pid ) ) );
		if ( !process )
		{
			return false;
		}
		static const auto current = userOf( GetCurrentProcess() );
		const auto        user    = userOf( process.get() );
		return !user.empty() && user == current;
	}

	bool isRunning( int pid, std::string_view name )
	{
		if ( pid <= 0 )
		{
			return false;
		}
		const Handle process( OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>( pid ) ) );
		DWORD        code = 0;
		if ( !process || GetExitCodeProcess( process.get(), &code ) == FALSE || code != STILL_ACTIVE )
		{
			return false;
		}
		std::wstring image( 32768, L'\0' );
		auto         size = static_cast<DWORD>( image.size() );
		if ( QueryFullProcessImageNameW( process.get(), 0, image.data(), &size ) == FALSE )
		{
			return false;
		}
		image.resize( size );
		return matches( image, name ) && sameUser( pid );
	}

	std::vector<int> othersNamed( std::string_view name )
	{
		std::vector<int> found;
		const Handle     snapshot( CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 ) );
		if ( !snapshot )
		{
			return found;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof( entry );
		for ( BOOL more = Process32FirstW( snapshot.get(), &entry ); more != FALSE; more = Process32NextW( snapshot.get(), &entry ) )
		{
			const auto pid = static_cast<int>( entry.th32ProcessID );
			if ( entry.th32ProcessID != GetCurrentProcessId() && matches( entry.szExeFile, name ) && isRunning( pid, name ) )
			{
				found.push_back( pid );
			}
		}
		return found;
	}

	bool terminate( int pid, std::string_view name, std::chrono::milliseconds grace )
	{
		if ( !isRunning( pid, name ) )
		{
			return true;
		}
		// Windows has no SIGTERM: the daemon was asked to exit over its pipe and gets `grace` to do so.
		const Handle process( OpenProcess( PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>( pid ) ) );
		if ( !process )
		{
			return false;
		}
		if ( WaitForSingleObject( process.get(), static_cast<DWORD>( std::max<long long>( 0, grace.count() ) ) ) == WAIT_OBJECT_0 )
		{
			return true;
		}
		TerminateProcess( process.get(), 1 );
		return WaitForSingleObject( process.get(), 2000 ) == WAIT_OBJECT_0;
	}

	fs::path executable()
	{
		std::wstring path( MAX_PATH, L'\0' );
		while ( true )
		{
			const DWORD length = GetModuleFileNameW( nullptr, path.data(), static_cast<DWORD>( path.size() ) );
			if ( length == 0 )
			{
				return {};
			}
			if ( length < path.size() )
			{
				path.resize( length );
				return path;
			}
			path.resize( path.size() * 2 );
		}
	}

	bool executableReplaced()
	{
		FILETIME                  created{};
		FILETIME                  exited{};
		FILETIME                  kernel{};
		FILETIME                  user{};
		WIN32_FILE_ATTRIBUTE_DATA file{};
		if ( GetProcessTimes( GetCurrentProcess(), &created, &exited, &kernel, &user ) == FALSE || GetFileAttributesExW( executable().c_str(), GetFileExInfoStandard, &file ) == FALSE )
		{
			return false;
		}
		const auto ticks = []( const FILETIME& time ) { return ( static_cast<unsigned long long>( time.dwHighDateTime ) << 32U ) | time.dwLowDateTime; };
		// A second of slack; FILETIME counts 100 ns.
		return ticks( file.ftLastWriteTime ) > ticks( created ) + 10'000'000ULL;
	}

#else

	namespace
	{

		// The fields of /proc/<pid>/stat after the command name, which may itself contain spaces and parentheses.
		std::vector<std::string> statFields( int pid )
		{
			const std::string stat  = readFile( fs::path( "/proc" ) / std::to_string( pid ) / "stat" );
			const auto        close = stat.rfind( ')' );
			if ( close == std::string::npos )
			{
				return {};
			}
			std::vector<std::string> fields;
			std::istringstream       rest( stat.substr( close + 1 ) );
			for ( std::string field; rest >> field; )
			{
				fields.push_back( std::move( field ) );
			}
			return fields;
		}

	} // namespace

	void InstanceLock::release() noexcept
	{
		if ( fd_ >= 0 )
		{
			// Closing the descriptor releases the lock; the file stays so the next holder reuses it.
			::close( fd_ );
			fd_ = -1;
		}
		held_ = false;
	}

	bool InstanceLock::retry()
	{
		if ( held_ )
		{
			return true;
		}
		if ( fd_ < 0 )
		{
			// Close-on-exec: an audio player started by the daemon must never keep holding the lock after it.
			fd_ = ::open( file_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600 ); // NOLINT(cppcoreguidelines-pro-type-vararg): open(2) takes the mode as a variadic argument
			if ( fd_ < 0 )
			{
				return false;
			}
		}
		if ( ::flock( fd_, LOCK_EX | LOCK_NB ) != 0 )
		{
			return false;
		}
		held_                   = true;
		const std::string pid   = std::to_string( ::getpid() ) + "\n";
		const bool        wrote = ::ftruncate( fd_, 0 ) == 0 && ::pwrite( fd_, pid.data(), pid.size(), 0 ) == static_cast<ssize_t>( pid.size() );
		( void )wrote;
		return true;
	}

	bool sameUser( int pid )
	{
		if ( pid <= 0 )
		{
			return false;
		}
		struct stat info{};
		if ( ::stat( ( fs::path( "/proc" ) / std::to_string( pid ) ).c_str(), &info ) == 0 )
		{
			return info.st_uid == ::getuid();
		}
		// Without /proc (macOS, BSD) a process one may signal is one's own.
		std::error_code ec;
		return !fs::exists( "/proc/self", ec ) && ::kill( pid, 0 ) == 0;
	}

	bool isRunning( int pid, std::string_view name )
	{
		if ( pid <= 0 || !sameUser( pid ) )
		{
			return false;
		}
		const fs::path dir  = fs::path( "/proc" ) / std::to_string( pid );
		std::string    comm = readFile( dir / "comm" );
		while ( !comm.empty() && ( comm.back() == '\n' || comm.back() == ' ' ) )
		{
			comm.pop_back();
		}
		// The kernel keeps at most 15 characters of the name.
		if ( comm != name.substr( 0, 15 ) )
		{
			return false;
		}
		const auto fields = statFields( pid );
		return !fields.empty() && fields.front() != "Z" && fields.front() != "X";
	}

	std::vector<int> othersNamed( std::string_view name )
	{
		std::vector<int> found;
		std::error_code  ec;
		for ( const auto& entry : fs::directory_iterator( "/proc", ec ) )
		{
			const std::string entry_name = entry.path().filename().string();
			int               pid        = 0;
			const auto [end, error]      = std::from_chars( entry_name.data(), entry_name.data() + entry_name.size(), pid );
			if ( error != std::errc() || end != entry_name.data() + entry_name.size() || pid == ::getpid() )
			{
				continue;
			}
			if ( isRunning( pid, name ) )
			{
				found.push_back( pid );
			}
		}
		return found;
	}

	bool terminate( int pid, std::string_view name, std::chrono::milliseconds grace )
	{
		const auto gone = [&] { return !isRunning( pid, name ); };
		if ( gone() )
		{
			return true;
		}
		( void )::kill( pid, SIGTERM );
		// A stopped process only acts on SIGTERM once it runs again.
		( void )::kill( pid, SIGCONT );
		if ( waitFor( gone, grace ) )
		{
			return true;
		}
		( void )::kill( pid, SIGKILL );
		return waitFor( gone, std::chrono::seconds( 2 ) );
	}

	fs::path executable()
	{
		std::error_code            ec;
		auto                       path    = fs::read_symlink( "/proc/self/exe", ec );
		std::string                native  = path.string();
		constexpr std::string_view deleted = " (deleted)";
		if ( native.ends_with( deleted ) )
		{
			native.resize( native.size() - deleted.size() );
			path = native;
		}
		return path;
	}

	bool executableReplaced()
	{
		std::error_code ec;
		// A new file written in place of the old one leaves the running image unlinked.
		if ( fs::read_symlink( "/proc/self/exe", ec ).string().ends_with( " (deleted)" ) )
		{
			return true;
		}
		// Otherwise compare the file's modification time with when this process started.
		const auto         fields = statFields( ::getpid() );
		std::istringstream boot_info( readFile( "/proc/stat" ) );
		long long          boot_time = 0;
		for ( std::string line; std::getline( boot_info, line ); )
		{
			if ( line.starts_with( "btime " ) )
			{
				boot_time = parseInt<long long>( std::string_view( line ).substr( 6 ) );
			}
		}
		// Field 22 of stat (the 20th after the name and state) is the start time in clock ticks after boot.
		if ( fields.size() < 20 || boot_time == 0 )
		{
			return false;
		}
		const auto      ticks   = parseInt<long long>( fields[19] );
		const long long started = boot_time + ( ticks / std::max( 1L, ::sysconf( _SC_CLK_TCK ) ) );
		struct stat     info{};
		if ( ::stat( executable().c_str(), &info ) != 0 )
		{
			return false;
		}
		return info.st_mtim.tv_sec > started + 1;
	}

#endif

} // namespace lexiglance::process
