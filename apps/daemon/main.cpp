#include "Daemon.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Thread.h>
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
	#include "Elevation.h"

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
#ifdef _WIN32
		std::println( "  --run-as-administrator on|off|status" );
		std::println( "               set up (run as administrator), remove or show the scheduled task that starts" );
		std::println( "               Lexiglance as administrator, for programs that run so (Overview -> Startup)" );
#endif
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

	// lexiglanced --run-as-administrator on|off|status [<account>]: what Overview -> Startup -> Run as administrator does
	// (Elevation.h). "on" runs as administrator; <account> (a SID) is the user it is for, since Windows may have given
	// the rights to another account (an administrator's password typed in for a standard user). Exit code 0 once done,
	// 3 when the rights went to another account.
	int runAsAdministrator( std::string_view request, std::string_view account )
	{
		namespace elevation = lg::daemon::elevation;
		if ( request == "status" )
		{
			const auto program = elevation::taskProgram();
			if ( !program )
			{
				std::println( stderr, "{}", program.error().message );
				return 1;
			}
			std::println( "{}", program->empty() ? std::string( "off" ) : std::format( "on: {}", program->string() ) );
			return 0;
		}
		if ( request == "off" )
		{
			if ( const auto removed = elevation::remove(); !removed )
			{
				std::println( stderr, "{}", removed.error().message );
				return 1;
			}
			return 0;
		}
		if ( request == "on" )
		{
			if ( !account.empty() && account != elevation::userSid() )
			{
				std::println( stderr, "administrator rights were given to another account ({}), not to {}", elevation::userSid(), account );
				return 3;
			}
			if ( !elevation::elevated() )
			{
				std::println( stderr, "setting it up takes administrator rights: run this as administrator" );
				return 1;
			}
			if ( const auto installed = elevation::install( lg::process::executable() ); !installed )
			{
				std::println( stderr, "{}", installed.error().message );
				return 1;
			}
			return 0;
		}
		usage();
		return 2;
	}

	// Run as administrator is set up for this program and this daemon runs without those rights (started at login, by
	// the settings application or a --replace): it starts the scheduled task and makes way for the daemon that starts.
	// True once that one runs; otherwise `problem` says why it did not, and this daemon runs as it is.
	bool handOver( lg::process::InstanceLock& lock, std::string& problem )
	{
		// The daemon the task starts takes the lock.
		lock               = lg::process::InstanceLock();
		const auto started = lg::daemon::elevation::start();
		// Quick as a rule; at login the Task Scheduler can take its time.
		if ( started && waitUntil( [] { return socketAnswers(); }, std::chrono::seconds( 20 ) ) )
		{
			return true;
		}
		problem = started ? std::string( "the scheduled task did not start it within 20 seconds" ) : started.error().message;
		lock    = lg::process::InstanceLock( lg::paths::runtimeDir() / "daemon.lock" );
		// Taken meanwhile: the daemon the task started is still getting ready.
		return !lock.held();
	}
#endif

	int run( std::span<char*> argv )
	{
#ifdef _WIN32
		attachConsole();
		// lexiglanced --run-as-administrator on|off|status [<account>] does only that.
		if ( argv.size() >= 3 && std::string_view( argv[1] ) == "--run-as-administrator" )
		{
			return runAsAdministrator( argv[2], argv.size() >= 4 ? std::string_view( argv[3] ) : std::string_view() );
		}
		// Started by Run as administrator's scheduled task, which does not start it again whatever it runs as.
		bool from_task = false;
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
#ifdef _WIN32
			else if ( arg == "--from-task" )
			{
				from_task = true;
			}
#endif
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
		lg::thread::setName( "daemon" );
		lg::log::setLevel( verbose ? lg::log::Level::Debug : lg::log::parseLevel( config->log_level ).value_or( lg::log::Level::Info ) );
		const auto log_file = lg::paths::stateDir() / "daemon.log";

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
#ifdef _WIN32
		// Run as administrator, set up for this program (Elevation.h): a daemon without those rights makes way for one
		// with them.
		const bool      elevated = lg::daemon::elevation::elevated();
		const auto      task     = lg::daemon::elevation::taskProgram();
		std::error_code same_error;
		const bool      task_here = task && !task->empty() && std::filesystem::equivalent( *task, lg::process::executable(), same_error );
		std::string     elevation_problem;
		if ( task_here && !elevated && !from_task && handOver( lock, elevation_problem ) )
		{
			return 0;
		}
#endif
		// Only the daemon that runs writes the log (and rotates it when it has grown too large).
		lg::log::setFile( log_file );
		lg::log::info( "---- lexiglanced {} ({} build{}) ----", lg::version, lg::channel, lg::commit.empty() ? std::string() : std::format( " {}", lg::commit ) );
		lg::log::info( "program {}, settings {}, dictionaries {}", lg::process::executable().string(), lg::paths::configFile().string(), lg::paths::dictionariesDir().string() );
		if ( !config_problem.empty() )
		{
			lg::log::error( "{} (using defaults)", config_problem );
		}
#ifdef _WIN32
		if ( elevated )
		{
			lg::log::info( "running as administrator{}", from_task ? " (started by the scheduled task of Run as administrator)" : "" );
		}
		else if ( from_task )
		{
			elevation_problem = "the scheduled task started it without administrator rights, which this account does not have";
		}
		if ( !elevation_problem.empty() )
		{
			lg::log::warn( "run as administrator: {}; running without administrator rights", elevation_problem );
		}
		else if ( task && !task->empty() && !task_here )
		{
			lg::log::info( "run as administrator is set up for another copy ({}); this one runs without administrator rights", task->string() );
		}
#endif

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
#ifdef _WIN32
		daemon.setAdministrator( elevated, task_here );
		if ( !elevation_problem.empty() )
		{
			daemon.noteStartupProblem( std::format( "Run as administrator is on, but Lexiglance could not start as administrator ({}), so the trigger does nothing over programs that run as administrator. Turn "
			                                        "Run as administrator off and on again (Overview, Startup).",
			                                        elevation_problem ) );
		}
#endif
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
