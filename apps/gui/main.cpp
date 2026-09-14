#include "DesktopEntry.h"
#include "MainWindow.h"

#include <lexiglance/core/Version.h>

#include <QApplication>
#include <QLoggingCategory>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <cstdio>
#include <exception>
#include <print>

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

	int run( int argc, char** argv )
	{
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

		// For documentation and checks: draws a page of the window into an image and exits (works offscreen, with
		// QT_QPA_PLATFORM=offscreen). --screenshot <page> <file.png> [milliseconds to wait for the daemon's data]
		if ( const auto shot = arguments.indexOf( QStringLiteral( "--screenshot" ) ); shot >= 0 && shot + 2 < arguments.size() )
		{
			lexiglance::gui::MainWindow window;
			window.resize( 1080, 780 );
			window.showPage( arguments[shot + 1] );
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

		// A second launch brings the running window to the front instead.
		const QString instance = QStringLiteral( "lexiglance-gui-%1" ).arg( userKey() );
		{
			QLocalSocket existing;
			existing.connectToServer( instance );
			if ( existing.waitForConnected( 250 ) )
			{
				existing.write( ( query.isEmpty() ? QStringLiteral( "show " ) + page : QStringLiteral( "find " ) + query ).append( QLatin1Char( '\n' ) ).toUtf8() );
				existing.waitForBytesWritten( 250 );
				return 0;
			}
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
			else
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
