#include "DesktopEntry.h"
#include "MainWindow.h"
#include "UpdateGroup.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/core/Version.h>

#include <QApplication>
#include <QLoggingCategory>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <unistd.h>
#endif

namespace
{

	// Tells apart the instances of different users on one machine.
	QString userKey()
	{
#ifdef _WIN32
		return qEnvironmentVariable( "USERNAME" );
#else
		return QString::number( ::getuid() );
#endif
	}

	// Qt's own warnings go into the log too, so a window that misbehaves leaves a trace behind.
	void logQtMessage( QtMsgType type, const QMessageLogContext& context, const QString& message )
	{
		const std::string text = context.category != nullptr && std::string_view( context.category ) != "default" ? std::format( "qt {}: {}", context.category, message.toStdString() ) : std::format( "qt: {}", message.toStdString() );
		switch ( type )
		{
			case QtDebugMsg:
				lexiglance::log::debug( "{}", text );
				return;
			case QtInfoMsg:
				lexiglance::log::info( "{}", text );
				return;
			case QtWarningMsg:
				lexiglance::log::warn( "{}", text );
				return;
			case QtCriticalMsg:
			case QtFatalMsg:
				lexiglance::log::error( "{}", text );
				return;
		}
	}

	// This window keeps a log of its own beside the daemon's: what it did, what it downloaded and what went wrong, so a
	// problem here can be looked into afterwards as well.
	void startLogging( const QStringList& arguments )
	{
		namespace lg = lexiglance;
		lg::thread::setName( "settings" );
		lg::log::setLevel( arguments.contains( QStringLiteral( "--verbose" ) ) || arguments.contains( QStringLiteral( "-v" ) ) ? lg::log::Level::Debug : lg::log::Level::Info );
		// Earlier builds called it settings.log; keep writing into the same file under the new name.
		const auto      log_file = lg::paths::stateDir() / "application.log";
		const auto      legacy   = lg::paths::stateDir() / "settings.log";
		std::error_code ec;
		if ( !std::filesystem::exists( log_file, ec ) && std::filesystem::exists( legacy, ec ) )
		{
			std::filesystem::rename( legacy, log_file, ec );
		}
		lg::log::setFile( log_file );
		lg::log::info( "---- lexiglance {} ({} build{}) ----", lg::version, lg::channel, lg::commit.empty() ? std::string() : std::format( " {}", lg::commit ) );
		lg::log::info(
				"program {}, {} ({}), Qt {}",
				lg::process::executable().string(),
				QSysInfo::prettyProductName().toStdString(),
				QSysInfo::currentCpuArchitecture().toStdString(),
				qVersion()
		);
		qInstallMessageHandler( &logQtMessage );
	}

	// lexiglance --daemon [arguments]: the daemon beside this program, in its place. An AppImage starts this program, so
	// that is how its login autostart starts the daemon; replacing the process keeps the AppImage mounted for it.
	int runDaemon( int argc, char** argv, int from )
	{
		namespace lg       = lexiglance;
		const auto program = lg::process::sibling( "lexiglanced" );
		if ( program.empty() )
		{
			std::println( stderr, "error: lexiglanced is not beside {}", lg::process::executable().string() );
			return 1;
		}
		std::vector<std::string> arguments( argv + from, argv + argc );
#ifdef _WIN32
		return lg::process::startDetached( program, arguments ) ? 0 : 1;
#else
		const std::string  path = program.string();
		std::vector<char*> list{ const_cast<char*>( path.c_str() ) };
		for ( std::string& argument : arguments )
		{
			list.push_back( argument.data() );
		}
		list.push_back( nullptr );
		::execv( path.c_str(), list.data() );
		std::println( stderr, "error: cannot start {}", path );
		return 1;
#endif
	}

	int run( int argc, char** argv )
	{
		for ( int i = 1; i < argc; ++i )
		{
			if ( std::string_view( argv[i] ) == "--daemon" )
			{
				return runDaemon( argc, argv, i + 1 );
			}
		}
#ifdef _WIN32
		// A windowed program has no console; messages (--menu-entry) go to the terminal it was started from, if any.
		if ( AttachConsole( ATTACH_PARENT_PROCESS ) != FALSE )
		{
			FILE* stream = nullptr;
			( void )freopen_s( &stream, "CONOUT$", "w", stdout );
			( void )freopen_s( &stream, "CONOUT$", "w", stderr );
		}
#endif
		// Qt reports two harmless things on every start: a second portal registration, and an AT-SPI method it does not
		// implement that accessibility clients (such as the daemon) ask for.
		if ( qEnvironmentVariableIsEmpty( "QT_LOGGING_RULES" ) )
		{
			QLoggingCategory::setFilterRules( QStringLiteral( "qt.qpa.services.warning=false\nqt.accessibility.atspi.warning=false" ) );
		}
		const QApplication app( argc, argv );
		QApplication::setApplicationName( QStringLiteral( "Lexiglance" ) );
		QApplication::setApplicationVersion( lexiglance::gui::qs( lexiglance::version ) );
		QApplication::setDesktopFileName( QStringLiteral( "io.github.mattfor.lexiglance" ) );
		QApplication::setStyle( QStringLiteral( "Fusion" ) );
		QApplication::setQuitOnLastWindowClosed( false );

		const QStringList arguments = QApplication::arguments();
		startLogging( arguments );

		// For documentation and checks: draws a page of the window into an image and exits (works offscreen, with
		// QT_QPA_PLATFORM=offscreen). --screenshot <page> <file.png> [milliseconds to wait for the daemon's data]
		if ( const auto shot = arguments.indexOf( QStringLiteral( "--screenshot" ) ); shot >= 0 && shot + 2 < arguments.size() )
		{
			lexiglance::gui::MainWindow window;
			window.resize( 1080, 780 );
			window.showPage( arguments[shot + 1] );
			// --search <text> along with it: the page is drawn with that looked up, as the documentation shows it.
			if ( const auto asked = arguments.indexOf( QStringLiteral( "--search" ) ); asked >= 0 && asked + 1 < arguments.size() )
			{
				window.search( arguments[asked + 1] );
			}
			const int wait = shot + 3 < arguments.size() ? arguments[shot + 3].toInt() : 3500;
			QTimer::singleShot( std::max( 100, wait ), &window, [&window, file = arguments[shot + 2]] {
				window.grab().save( file );
				QApplication::quit();
			} );
			return QApplication::exec();
		}

		// lexiglance --menu-entry on|off: adds this program to the applications menu or takes it out (for scripts).
		if ( const auto menu = arguments.indexOf( QStringLiteral( "--menu-entry" ) ); menu >= 0 && menu + 1 < arguments.size() )
		{
			QString    error;
			const bool shown = arguments[menu + 1] != QStringLiteral( "off" );
			if ( !lexiglance::gui::desktop::setMenuEntry( shown, &error ) )
			{
				std::println( stderr, "error: {}", error.toStdString() );
				return 1;
			}
			std::println( "Lexiglance is {} the applications menu", shown ? "in" : "no longer in" );
			return 0;
		}

		const auto    page_flag   = arguments.indexOf( QStringLiteral( "--page" ) );
		const QString page        = page_flag >= 0 && page_flag + 1 < arguments.size() ? arguments[page_flag + 1] : QString();
		const auto    search_flag = arguments.indexOf( QStringLiteral( "--search" ) );
		const QString query       = search_flag >= 0 && search_flag + 1 < arguments.size() ? arguments[search_flag + 1] : QString();

		// A second launch brings the running window to the front instead. One started by an update (--updated) waits
		// for the version it replaces to end.
		const QString instance = QStringLiteral( "lexiglance-gui-%1" ).arg( userKey() );

		// lexiglance --background-update: started by the daemon now and then, so a copy updates itself even when this
		// window is never opened. It shows nothing, and leaves updating to a window that is open (which updates on its
		// own); it does not count as one either, so opening the window meanwhile opens it as usual.
		if ( arguments.contains( QStringLiteral( "--background-update" ) ) )
		{
			QLocalSocket existing;
			existing.connectToServer( instance );
			if ( existing.waitForConnected( 250 ) )
			{
				lexiglance::log::info( "update: the settings window is open and updates by itself" );
				return 0;
			}
			const lexiglance::gui::UpdateGroup updater( nullptr, true );
			return QApplication::exec();
		}
		const bool updated = arguments.contains( QStringLiteral( "--updated" ) );
		for ( int waited = 0;; ++waited )
		{
			QLocalSocket existing;
			existing.connectToServer( instance );
			if ( !existing.waitForConnected( 250 ) )
			{
				break;
			}
			if ( !updated || waited >= 40 )
			{
				const QString request = query.isEmpty() ? QStringLiteral( "show %1\n" ).arg( page ) : QStringLiteral( "find %1\n" ).arg( query );
				lexiglance::log::info( "another settings window is open: asked it to {}", request.trimmed().toStdString() );
				existing.write( request.toUtf8() );
				existing.waitForBytesWritten( 250 );
				return 0;
			}
			existing.disconnectFromServer();
			QThread::msleep( 250 );
		}
		QLocalServer::removeServer( instance );
		QLocalServer server;
		server.listen( instance );

		lexiglance::gui::MainWindow window;
		// In the applications menu from the first start on, also when run from a build tree or an AppImage.
		if ( QGuiApplication::platformName() != QStringLiteral( "offscreen" ) )
		{
			lexiglance::gui::desktop::maintainMenuEntry();
		}
		QObject::connect( &server, &QLocalServer::newConnection, &window, [&server, &window] {
			QString requested;
			if ( QLocalSocket* connection = server.nextPendingConnection() )
			{
				if ( connection->waitForReadyRead( 200 ) )
				{
					requested = QString::fromUtf8( connection->readAll() ).trimmed();
				}
				connection->deleteLater();
			}
			if ( requested.startsWith( QStringLiteral( "find " ) ) )
			{
				window.search( requested.mid( 5 ) );
			}
			// A copy that only looked whether this window is open (--background-update, or --updated waiting for this
			// one to end) sends nothing: that is not a request to come to the front.
			else if ( !requested.isEmpty() )
			{
				window.showPage( requested.mid( 5 ).trimmed() );
			}
		} );

		if ( !arguments.contains( QStringLiteral( "--tray" ) ) )
		{
			if ( query.isEmpty() )
			{
				window.showPage( page );
			}
			else
			{
				window.search( query );
			}
			window.welcome();
		}
		return QApplication::exec();
	}

} // namespace

int main( int argc, char** argv )
{
	try
	{
		return run( argc, argv );
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
