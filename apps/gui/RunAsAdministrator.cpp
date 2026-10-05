#include "RunAsAdministrator.h"

#include "Common.h"

#include <lexiglance/core/Log.h>

#include <QDir>
#include <QProcess>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	// After windows.h, which they need.
	#include <sddl.h>
	#include <shellapi.h>

	#include <vector>
#endif

namespace lexiglance::gui::administrator
{

#ifdef Q_OS_WIN

	namespace
	{

		// This user's account (a SID): the daemon program checks that the administrator rights Windows gave it are this
		// account's, not another's whose password was typed in.
		QString userSid()
		{
			HANDLE token = nullptr;
			if ( OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ) == FALSE )
			{
				return {};
			}
			DWORD size = 0;
			GetTokenInformation( token, TokenUser, nullptr, 0, &size );
			std::vector<unsigned char> buffer( size );
			QString                    sid;
			LPWSTR                     text = nullptr;
			if ( size > 0 && GetTokenInformation( token, TokenUser, buffer.data(), size, &size ) != FALSE && ConvertSidToStringSidW( reinterpret_cast<const TOKEN_USER*>( buffer.data() )->User.Sid, &text ) != FALSE )
			{
				sid = QString::fromWCharArray( text );
				LocalFree( text );
			}
			CloseHandle( token );
			return sid;
		}

		// The daemon program with `arguments`, as administrator: Windows asks first. `done` gets its exit code, or -1 and
		// why it did not run.
		void runElevated( QWidget* owner, const QString& arguments, const std::function<void( int, const QString& )>& done )
		{
			const std::wstring program    = QDir::toNativeSeparators( daemonExecutable() ).toStdWString();
			const std::wstring parameters = arguments.toStdWString();
			SHELLEXECUTEINFOW  info{};
			info.cbSize       = sizeof( info );
			info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
			info.hwnd         = owner != nullptr ? reinterpret_cast<HWND>( owner->window()->winId() ) : nullptr;
			info.lpVerb       = L"runas";
			info.lpFile       = program.c_str();
			info.lpParameters = parameters.c_str();
			info.nShow        = SW_HIDE;
			// Returns once Windows' prompt was answered.
			if ( ShellExecuteExW( &info ) == FALSE || info.hProcess == nullptr )
			{
				const DWORD error = GetLastError();
				done( -1, error == ERROR_CANCELLED ? QStringLiteral( "Windows did not give administrator rights, so nothing was changed." ) : QStringLiteral( "Cannot start %1 as administrator (error %2)." ).arg( daemonExecutable() ).arg( error ) );
				return;
			}
			// Waited for without blocking the window; it takes a moment.
			const HANDLE process = info.hProcess;
			auto*        timer   = new QTimer( owner );
			QObject::connect( timer, &QTimer::timeout, owner, [timer, process, done, ticks = 0]() mutable {
				if ( WaitForSingleObject( process, 0 ) == WAIT_TIMEOUT && ++ticks < 600 )
				{
					return;
				}
				timer->stop();
				timer->deleteLater();
				DWORD      code  = 0;
				const bool ended = GetExitCodeProcess( process, &code ) != FALSE && code != STILL_ACTIVE;
				CloseHandle( process );
				done( ended ? static_cast<int>( code ) : -1, ended ? QString() : QStringLiteral( "It did not finish within a minute." ) );
			} );
			timer->start( 100 );
		}

	} // namespace

	bool available()
	{
		return true;
	}

	void set( QWidget* owner, bool on, const std::function<void( const QString& )>& done )
	{
		if ( on )
		{
			log::info( "run as administrator: setting it up (Windows asks for administrator rights)" );
			runElevated( owner, QStringLiteral( "--run-as-administrator on %1" ).arg( userSid() ), [done]( int code, const QString& problem ) {
				if ( code == 0 )
				{
					log::info( "run as administrator: set up" );
					done( {} );
					return;
				}
				QString message = problem;
				if ( code == 3 )
				{
					message = QStringLiteral( "Windows gave the administrator rights to another account. Run as administrator needs this account to be an administrator." );
				}
				else if ( message.isEmpty() )
				{
					message = QStringLiteral( "Setting it up failed (exit code %1)." ).arg( code );
				}
				log::warn( "run as administrator: not set up: {}", ss( message ) );
				done( message );
			} );
			return;
		}
		auto* process = new QProcess( owner );
		QObject::connect( process, &QProcess::errorOccurred, owner, [process, done]( QProcess::ProcessError error ) {
			if ( error == QProcess::FailedToStart )
			{
				process->deleteLater();
				done( QStringLiteral( "Cannot start %1." ).arg( daemonExecutable() ) );
			}
		} );
		QObject::connect( process, &QProcess::finished, owner, [owner, process, done]( int code, QProcess::ExitStatus status ) {
			process->deleteLater();
			if ( status == QProcess::NormalExit && code == 0 )
			{
				log::info( "run as administrator: turned off" );
				done( {} );
				return;
			}
			// A task this user may not delete (one set up by hand, say): as administrator then.
			runElevated( owner, QStringLiteral( "--run-as-administrator off" ), [done]( int elevated_code, const QString& problem ) {
				if ( elevated_code == 0 )
				{
					log::info( "run as administrator: turned off" );
					done( {} );
					return;
				}
				done( problem.isEmpty() ? QStringLiteral( "Removing it failed (exit code %1)." ).arg( elevated_code ) : problem );
			} );
		} );
		process->start( daemonExecutable(), { QStringLiteral( "--run-as-administrator" ), QStringLiteral( "off" ) } );
	}

	void removeNow()
	{
		QProcess process;
		process.start( daemonExecutable(), { QStringLiteral( "--run-as-administrator" ), QStringLiteral( "off" ) } );
		if ( process.waitForStarted( 3000 ) )
		{
			process.waitForFinished( 10000 );
		}
	}

#else

	bool available()
	{
		return false;
	}

	void set( QWidget*, bool, const std::function<void( const QString& )>& done )
	{
		done( QStringLiteral( "Run as administrator is there only on Windows." ) );
	}

	void removeNow() {}

#endif

} // namespace lexiglance::gui::administrator
