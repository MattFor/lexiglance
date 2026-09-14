#include "Daemon.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/ipc/Socket.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string_view>
#include <thread>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <pthread.h>
#endif

namespace
{

	namespace lg = lexiglance;

	constexpr std::string_view daemon_name = "lexiglanced";
	// How long tearing down may take before the process simply ends.
	constexpr auto shutdown_limit = std::chrono::seconds( 4 );

	void usage()
	{
		std::println( "lexiglanced {} - Lexiglance lookup daemon", lg::version );
		std::println( "" );
		std::println( "  --replace    stop a running daemon (ending it if it hangs) and take over" );
		std::println( "  --verbose    debug logging" );
		std::println( "  --version    print the version and exit" );
	}

	template <typename Predicate>
	bool waitUntil( Predicate done, std::chrono::milliseconds timeout )
	{
		const auto until = std::chrono::steady_clock::now() + timeout;
		while ( !done() )
		{
			if ( std::chrono::steady_clock::now() >= until )
			{
				return false;
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		}
		return true;
	}

	bool socketAnswers()
	{
		return lg::ipc::connectTo( lg::paths::ipcEndpoint(), std::chrono::milliseconds( 300 ) ).has_value();
	}

	// Takes over from a running daemon: asks it to exit, ends it when it does not, and ends stray daemons too (from
	// before the instance lock, or ones that lost their socket), which would keep showing popups of their own.
	bool replaceRunning( lg::process::InstanceLock& lock )
	{
		if ( auto client = lg::ipc::Client::connect( lg::paths::ipcEndpoint(), std::chrono::milliseconds( 1500 ) ) )
		{
			( void )client->call( "shutdown" );
		}
		if ( !waitUntil( [&] { return lock.retry(); }, shutdown_limit + std::chrono::seconds( 1 ) ) )
		{
			const int holder = lock.holder();
			lg::log::warn( "the running daemon (pid {}) did not exit; ending it", holder );
			if ( holder > 0 )
			{
				( void )lg::process::terminate( holder, daemon_name, std::chrono::seconds( 2 ) );
			}
			if ( !waitUntil( [&] { return lock.retry(); }, std::chrono::seconds( 3 ) ) )
			{
				return false;
			}
		}
		for ( const int pid : lg::process::othersNamed( daemon_name ) )
		{
			// One that had the socket was asked to exit above: a moment for it to do so on its own.
			if ( !waitUntil( [pid] { return !lg::process::isRunning( pid, daemon_name ); }, std::chrono::seconds( 3 ) ) )
			{
				lg::log::warn( "ending a stray daemon (pid {})", pid );
				( void )lg::process::terminate( pid, daemon_name, std::chrono::seconds( 2 ) );
			}
		}
		// The socket goes with its daemon; a file left behind is replaced when listening.
		( void )waitUntil( [] { return !socketAnswers(); }, std::chrono::seconds( 2 ) );
		return true;
	}

	void rotateLog( const std::filesystem::path& file )
	{
		std::error_code ec;
		if ( std::filesystem::file_size( file, ec ) > 2U << 20U )
		{
			auto old = file;
			old += ".old";
			std::filesystem::rename( file, old, ec );
		}
	}

#ifdef _WIN32
	// The daemon asked to quit when its console is closed or interrupted (logging off ends the desktop backend itself).
	std::atomic<lg::daemon::Daemon*> running{ nullptr };

	BOOL WINAPI onConsoleEvent( DWORD event )
	{
		auto* daemon = running.load();
		if ( daemon == nullptr )
		{
			// Tearing down already: another interruption ends the process at once.
			return FALSE;
		}
		daemon->requestQuit();
		// Windows ends the process as soon as this returns from these events: time for the teardown first.
		if ( event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT )
		{
			std::this_thread::sleep_for( shutdown_limit );
		}
		return TRUE;
	}

	// The daemon has no window of its own (no console pops up when it starts); its output goes to the terminal it was
	// started from, if any.
	void attachConsole()
	{
		if ( AttachConsole( ATTACH_PARENT_PROCESS ) != FALSE )
		{
			FILE* stream = nullptr;
			( void )freopen_s( &stream, "CONOUT$", "w", stdout );
			( void )freopen_s( &stream, "CONOUT$", "w", stderr );
		}
	}
#endif

	int run( std::span<char*> argv )
	{
#ifdef _WIN32
		attachConsole();
#endif
		bool verbose = false;
		bool replace = false;
		for ( const std::string_view arg : argv.subspan( 1 ) )
		{
			if ( arg == "--verbose" || arg == "-v" )
			{
				verbose = true;
			}
			else if ( arg == "--replace" )
			{
				replace = true;
			}
			else if ( arg == "--version" )
			{
				std::println( "{}", lg::version );
				return 0;
			}
			else
			{
				usage();
				return arg == "--help" || arg == "-h" ? 0 : 2;
			}
		}

		// Tesseract parallelises with OpenMP; one thread keeps OCR from competing with games.
#ifdef _WIN32
		if ( std::getenv( "OMP_THREAD_LIMIT" ) == nullptr )
		{
			( void )_putenv_s( "OMP_THREAD_LIMIT", "1" );
		}
#else
		( void )::setenv( "OMP_THREAD_LIMIT", "1", 0 );
#endif

		auto        config = lg::config::Config::load( lg::paths::configFile() );
		std::string config_problem;
		if ( !config )
		{
			config_problem = config.error().message;
			config         = lg::config::Config{};
		}
		lg::log::setLevel( verbose ? lg::log::Level::Debug : lg::log::parseLevel( config->log_level ).value_or( lg::log::Level::Info ) );
		const auto log_file = lg::paths::stateDir() / "daemon.log";
		lg::log::setFile( log_file );

		// One daemon per user, decided before anything touches the desktop.
		( void )lg::paths::ensureDirectory( lg::paths::runtimeDir(), true );
		lg::process::InstanceLock lock( lg::paths::runtimeDir() / "daemon.lock" );
		if ( replace )
		{
			if ( !replaceRunning( lock ) )
			{
				lg::log::error( "the running daemon (pid {}) could not be stopped", lock.holder() );
				return 1;
			}
		}
		else if ( !lock.held() )
		{
			lg::log::error( "Lexiglance is already running (pid {}); `lexiglanced --replace` restarts it", lock.holder() );
			return 1;
		}
		// Only the daemon that runs rotates the log, closed meanwhile (Windows cannot rename an open file).
		lg::log::setFile( {} );
		rotateLog( log_file );
		lg::log::setFile( log_file );
		if ( !config_problem.empty() )
		{
			lg::log::error( "{} (using defaults)", config_problem );
		}

#ifndef _WIN32
		// Signals are received by one dedicated thread instead of interrupting arbitrary ones.
		sigset_t signals{};
		sigemptyset( &signals );
		sigaddset( &signals, SIGINT );
		sigaddset( &signals, SIGTERM );
		sigaddset( &signals, SIGHUP );
		pthread_sigmask( SIG_BLOCK, &signals, nullptr );
		( void )std::signal( SIGPIPE, SIG_IGN );
#endif

		auto        backend = lg::platform::createBackend();
		std::string backend_problem;
		if ( !backend )
		{
			backend_problem = backend.error().message;
			lg::log::warn( "{}", backend_problem );
		}

		lg::daemon::Daemon daemon( backend ? std::move( *backend ) : nullptr, std::move( *config ) );
		if ( !config_problem.empty() )
		{
			daemon.noteStartupProblem( std::format( "The settings file could not be read ({}), so the defaults are used. Fix or delete it.", config_problem ) );
		}
		if ( !backend_problem.empty() )
		{
			daemon.noteStartupProblem( std::format( "No desktop integration: {}.", backend_problem ) );
		}

#ifdef _WIN32
		running.store( &daemon );
		SetConsoleCtrlHandler( &onConsoleEvent, TRUE );
		const int code = daemon.run();
		running.store( nullptr );

		// Tearing down is bounded: a thread stuck in a call to a hung application must not keep a dead daemon (and its
		// lock) around.
		std::thread( [] {
			std::this_thread::sleep_for( shutdown_limit );
			lg::log::warn( "shutting down took too long; exiting" );
			std::_Exit( 0 );
		} ).detach();
		return code;
#else
		const std::jthread signal_thread( [&daemon, signals]( const std::stop_token& stop ) {
			const timespec interval{ .tv_sec = 0, .tv_nsec = 200'000'000 };
			while ( !stop.stop_requested() )
			{
				if ( sigtimedwait( &signals, nullptr, &interval ) > 0 )
				{
					daemon.requestQuit();
					return;
				}
			}
		} );

		const int code = daemon.run();

		// Tearing down is bounded: a thread stuck in a call to a hung application must not keep a dead daemon (and its
		// lock) around, and another termination signal meanwhile ends the process at once.
		std::thread( [signals] {
			const auto until = std::chrono::steady_clock::now() + shutdown_limit;
			while ( std::chrono::steady_clock::now() < until )
			{
				const timespec interval{ .tv_sec = 0, .tv_nsec = 100'000'000 };
				if ( sigtimedwait( &signals, nullptr, &interval ) > 0 )
				{
					std::_Exit( 0 );
				}
			}
			lg::log::warn( "shutting down took too long; exiting" );
			std::_Exit( 0 );
		} ).detach();
		return code;
#endif
	}

} // namespace

int main( int argc, char** argv )
{
	try
	{
		return run( std::span<char*>( argv, static_cast<std::size_t>( argc ) ) );
	}
	catch ( const std::exception& e )
	{
		( void )std::fputs( "fatal: ", stderr );
		( void )std::fputs( e.what(), stderr );
		( void )std::fputs( "\n", stderr );
	}
	catch ( ... )
	{
		( void )std::fputs( "fatal: unknown error\n", stderr );
	}
	return 1;
}
