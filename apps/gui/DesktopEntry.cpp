#include "DesktopEntry.h"

#include "Common.h"

#include <lexiglance/core/Log.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace lexiglance::gui::desktop
{

	namespace
	{

		// The program a command line starts: "/a path/program" or /path/program, with arguments or without.
		QString commandProgram( const QString& command )
		{
			static const QRegularExpression pattern( QStringLiteral( R"re(^\s*(?:"([^"]+)"|(\S+)))re" ) );
			const auto                      match = pattern.match( command );
			if ( !match.hasMatch() )
			{
				return {};
			}
			return match.captured( 1 ).isEmpty() ? match.captured( 2 ) : match.captured( 1 );
		}

		// Nothing starts from a program that is gone, or from inside the mount of an AppImage (which earlier versions
		// wrote, and which disappears when the AppImage ends).
		bool startsNothing( const QString& program )
		{
			return program.isEmpty() || !QFileInfo::exists( program ) || program.contains( QStringLiteral( "/.mount_" ) );
		}

		// The entries that are files (not on Windows, which keeps them in the registry and a shortcut).
		[[maybe_unused]] QString read( const QString& path )
		{
			QFile file( path );
			return file.open( QIODevice::ReadOnly ) ? QString::fromUtf8( file.readAll() ) : QString();
		}

		[[maybe_unused]] bool save( const QString& path, const QString& text, QString* error = nullptr )
		{
			QDir().mkpath( QFileInfo( path ).absolutePath() );
			QSaveFile file( path );
			if ( !file.open( QIODevice::WriteOnly ) || file.write( text.toUtf8() ) < 0 || !file.commit() )
			{
				if ( error != nullptr )
				{
					*error = QStringLiteral( "Cannot write %1" ).arg( QDir::toNativeSeparators( path ) );
				}
				return false;
			}
			return true;
		}

		void claimed( const char* what, const QString& before )
		{
			log::info( "entries: the {} started {}; it starts this copy ({}) now", what, before.isEmpty() ? std::string( "nothing" ) : ss( before ), ss( thisCopy() ) );
		}

		void mended( const char* what, const QString& before )
		{
			log::info( "entries: the {} started {}, which is gone; it starts this copy ({}) now", what, before.isEmpty() ? std::string( "nothing" ) : ss( before ), ss( thisCopy() ) );
		}

		// Into the menu from the first start on, also when run from a build tree or an AppImage; never again once taken out.
		[[maybe_unused]] void addMenuEntryOnce()
		{
			auto settings = applicationMemory();
			if ( settings.value( QStringLiteral( "menu/decided" ), false ).toBool() )
			{
				return;
			}
			settings.setValue( QStringLiteral( "menu/decided" ), true );
			if ( !menuEntryShown() )
			{
				( void )setMenuEntry( true );
			}
		}

		void mend();

	} // namespace

	QString thisCopy()
	{
		const QString appimage = qEnvironmentVariable( "APPIMAGE" );
		return appimage.isEmpty() ? daemonExecutable() : appimage;
	}

	bool sameCopy( const QString& a, const QString& b )
	{
		// QFileInfo compares the files themselves: links followed, and without case on Windows.
		return !a.isEmpty() && !b.isEmpty() && QFileInfo( a ) == QFileInfo( b );
	}

	bool inBuildTree()
	{
		// apps/gui/lexiglance beside apps/daemon/lexiglanced, where an install has bin/ for both.
		return QDir( QCoreApplication::applicationDirPath() ).exists( QStringLiteral( "../daemon" ) );
	}

	bool maintain()
	{
		if ( !qEnvironmentVariableIsEmpty( "LEXIGLANCE_HOME" ) )
		{
			return false;
		}
		if ( inBuildTree() )
		{
			mend();
			return false;
		}
		claim();
		return true;
	}

#ifdef Q_OS_MACOS

	namespace
	{

		QString autostartFile()
		{
			return QDir::homePath() + "/Library/LaunchAgents/io.github.mattfor.lexiglance.daemon.plist";
		}

		QString autostartText()
		{
			return QStringLiteral(
						   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
						   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
						   "<plist version=\"1.0\"><dict><key>Label</key><string>io.github.mattfor.lexiglance.daemon</string>"
						   "<key>ProgramArguments</key><array><string>%1</string></array><key>RunAtLoad</key><true/></dict></plist>\n"
			)
			        .arg( daemonExecutable().toHtmlEscaped() );
		}

		// The first of the ProgramArguments.
		QString autostartProgram()
		{
			static const QRegularExpression pattern( QStringLiteral( "<array><string>([^<]*)</string>" ) );
			return pattern.match( read( autostartFile() ) ).captured( 1 );
		}

		void mend()
		{
			if ( autostartEnabled() && startsNothing( autostartProgram() ) )
			{
				mended( "autostart", autostartProgram() );
				( void )setAutostart( true );
			}
		}

	} // namespace

	bool menuEntryShown()
	{
		return false;
	}

	bool setMenuEntry( bool, QString* error )
	{
		if ( error != nullptr )
		{
			*error = QStringLiteral( "Menu entries are made by the installer on this system." );
		}
		return false;
	}

	bool autostartEnabled()
	{
		return QFile::exists( autostartFile() );
	}

	bool setAutostart( bool enabled )
	{
		if ( !enabled )
		{
			return QFile::remove( autostartFile() ) || !QFile::exists( autostartFile() );
		}
		return save( autostartFile(), autostartText() );
	}

	void claim()
	{
		if ( autostartEnabled() && read( autostartFile() ) != autostartText() )
		{
			claimed( "autostart", autostartProgram() );
			( void )setAutostart( true );
		}
	}

#elifdef Q_OS_WIN

	namespace
	{

		constexpr auto daemon_value = "Lexiglance";
		constexpr auto tray_value   = "Lexiglance tray";

		QSettings runKey()
		{
			return { QStringLiteral( "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" ), QSettings::NativeFormat };
		}

		// A shortcut in this user's Start menu, where the setup puts its own.
		QString shortcut()
		{
			return QStandardPaths::writableLocation( QStandardPaths::ApplicationsLocation ) + QStringLiteral( "/Lexiglance.lnk" );
		}

		QString shortcutTarget()
		{
			return QFileInfo( shortcut() ).symLinkTarget();
		}

		QString program()
		{
			return QFileInfo( QCoreApplication::applicationFilePath() ).canonicalFilePath();
		}

		QString daemonCommand()
		{
			return QStringLiteral( "\"%1\"" ).arg( QDir::toNativeSeparators( daemonExecutable() ) );
		}

		QString trayCommand()
		{
			return QStringLiteral( "\"%1\" --tray" ).arg( QDir::toNativeSeparators( program() ) );
		}

		QString runValue( const char* name )
		{
			return runKey().value( QLatin1String( name ) ).toString();
		}

		void mend()
		{
			if ( autostartEnabled() && startsNothing( commandProgram( runValue( daemon_value ) ) ) )
			{
				mended( "autostart", commandProgram( runValue( daemon_value ) ) );
				( void )setAutostart( true );
			}
			if ( trayAutostartEnabled() && startsNothing( commandProgram( runValue( tray_value ) ) ) )
			{
				mended( "tray's autostart", commandProgram( runValue( tray_value ) ) );
				( void )setTrayAutostart( true );
			}
			if ( QFile::exists( shortcut() ) && startsNothing( shortcutTarget() ) )
			{
				mended( "Start menu shortcut", shortcutTarget() );
				( void )setMenuEntry( true );
			}
			addMenuEntryOnce();
		}

	} // namespace

	bool menuEntryShown()
	{
		return QFile::exists( shortcut() );
	}

	bool setMenuEntry( bool shown, QString* error )
	{
		auto settings = applicationMemory();
		settings.setValue( QStringLiteral( "menu/decided" ), true );
		if ( QFile::exists( shortcut() ) && !QFile::remove( shortcut() ) )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot remove %1" ).arg( QDir::toNativeSeparators( shortcut() ) );
			}
			return false;
		}
		// On Windows QFile::link() makes a shortcut (.lnk).
		if ( shown && ( !QDir().mkpath( QFileInfo( shortcut() ).absolutePath() ) || !QFile::link( program(), shortcut() ) ) )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot write %1" ).arg( QDir::toNativeSeparators( shortcut() ) );
			}
			return false;
		}
		return true;
	}

	bool autostartEnabled()
	{
		return runKey().contains( QLatin1String( daemon_value ) );
	}

	bool setAutostart( bool enabled )
	{
		auto run = runKey();
		if ( enabled )
		{
			run.setValue( QLatin1String( daemon_value ), daemonCommand() );
		}
		else
		{
			run.remove( QLatin1String( daemon_value ) );
		}
		run.sync();
		return run.status() == QSettings::NoError;
	}

	bool trayAutostartEnabled()
	{
		return runKey().contains( QLatin1String( tray_value ) );
	}

	bool setTrayAutostart( bool enabled )
	{
		auto run = runKey();
		if ( enabled )
		{
			run.setValue( QLatin1String( tray_value ), trayCommand() );
		}
		else
		{
			run.remove( QLatin1String( tray_value ) );
		}
		run.sync();
		return run.status() == QSettings::NoError;
	}

	void claim()
	{
		if ( autostartEnabled() && !sameCopy( commandProgram( runValue( daemon_value ) ), daemonExecutable() ) )
		{
			claimed( "autostart", commandProgram( runValue( daemon_value ) ) );
			( void )setAutostart( true );
		}
		if ( trayAutostartEnabled() && !sameCopy( commandProgram( runValue( tray_value ) ), program() ) )
		{
			claimed( "tray's autostart", commandProgram( runValue( tray_value ) ) );
			( void )setTrayAutostart( true );
		}
		if ( QFile::exists( shortcut() ) && !sameCopy( shortcutTarget(), program() ) )
		{
			claimed( "Start menu shortcut", shortcutTarget() );
			( void )setMenuEntry( true );
		}
		addMenuEntryOnce();
	}

#else

	namespace
	{

		const QString entry_name = QStringLiteral( "io.github.mattfor.lexiglance.desktop" );
		// Marks entries this program wrote (and may rewrite).
		const QString generated_key = QStringLiteral( "X-Lexiglance-Generated=true" );

		QString userEntry()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/applications/" + entry_name;
		}

		QString autostartFile()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-daemon.desktop";
		}

		QString trayAutostartFile()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-tray.desktop";
		}

		// Entries installed elsewhere (a package), which a user entry of the same name overrides.
		bool installedEntry()
		{
			return std::ranges::any_of( QStandardPaths::locateAll( QStandardPaths::GenericDataLocation, "applications/" + entry_name ), []( const QString& path ) { return QFileInfo( path ) != QFileInfo( userEntry() ); } );
		}

		bool hides( const QString& entry )
		{
			return entry.contains( QStringLiteral( "\nNoDisplay=true" ) ) || entry.contains( QStringLiteral( "\nHidden=true" ) );
		}

		// The program a desktop entry starts, from its (first) Exec line.
		QString entryProgram( const QString& entry )
		{
			static const QRegularExpression exec( QStringLiteral( "^Exec=(.*)$" ), QRegularExpression::MultilineOption );
			return commandProgram( exec.match( entry ).captured( 1 ) );
		}

		// What to start: the AppImage file itself rather than its mount, which changes every run.
		QString program()
		{
			const QString appimage = qEnvironmentVariable( "APPIMAGE" );
			return appimage.isEmpty() ? QCoreApplication::applicationFilePath() : appimage;
		}

		// An Exec argument, quoted as the desktop entry specification asks.
		QString quoted( QString argument )
		{
			for ( const QString& special : { QStringLiteral( "\\" ), QStringLiteral( "\"" ), QStringLiteral( "`" ), QStringLiteral( "$" ) } )
			{
				argument.replace( special, "\\" + special );
			}
			return "\"" + argument + "\"";
		}

		// The icon, from the program's resources, where the menu can find it.
		QString installIcon()
		{
			const QString path = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/icons/hicolor/scalable/apps/lexiglance.svg";
			if ( !QFile::exists( path ) )
			{
				QDir().mkpath( QFileInfo( path ).absolutePath() );
				QFile::copy( QStringLiteral( ":/lexiglance/lexiglance.svg" ), path );
				QFile::setPermissions( path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther );
			}
			return QFile::exists( path ) ? path : QStringLiteral( "accessories-dictionary" );
		}

		QString entryText()
		{
			const QString exec = quoted( program() );
			return QStringLiteral(
						   "[Desktop Entry]\n"
						   "Type=Application\n"
						   "Name=Lexiglance\n"
						   "GenericName=Pop-up Dictionary\n"
						   "Comment=System-wide pop-up dictionary for Yomitan dictionaries\n"
						   "Exec=%1\n"
						   "TryExec=%2\n"
						   "Icon=%3\n"
						   "Terminal=false\n"
						   "Categories=Education;Languages;\n"
						   "Keywords=dictionary;japanese;russian;ukrainian;korean;greek;yomitan;popup;translate;\n"
						   "StartupNotify=true\n"
						   "Actions=health;search;\n"
						   "%4\n"
						   "\n"
						   "[Desktop Action health]\n"
						   "Name=Check health\n"
						   "Exec=%1 --page overview\n"
						   "\n"
						   "[Desktop Action search]\n"
						   "Name=Search\n"
						   "Exec=%1 --page search\n"
			)
			        .arg( exec, program(), installIcon(), generated_key );
		}

		QString autostartText()
		{
			// From an AppImage, the AppImage file with --daemon: the daemon inside it lives in a mount that is gone once it
			// ends, so its own path would start nothing at the next login.
			const QString appimage = qEnvironmentVariable( "APPIMAGE" );
			const QString exec     = appimage.isEmpty() ? quoted( daemonExecutable() ) : quoted( appimage ) + QStringLiteral( " --daemon" );
			return QStringLiteral(
						   "[Desktop Entry]\nType=Application\nName=Lexiglance\nComment=System-wide pop-up dictionary\nExec=%1\n"
						   "Icon=lexiglance\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"
			)
			        .arg( exec );
		}

		QString trayAutostartText()
		{
			return QStringLiteral(
						   "[Desktop Entry]\nType=Application\nName=Lexiglance (tray)\nComment=The Lexiglance settings application, in the tray\n"
						   "Exec=%1 --tray\nIcon=lexiglance\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"
			)
			        .arg( quoted( program() ) );
		}

		// A menu entry this program wrote, and not one taking Lexiglance out of the menu: the one to keep pointing here.
		bool ownMenuEntry( const QString& entry )
		{
			return entry.contains( generated_key ) && !hides( entry );
		}

		void mend()
		{
			if ( const QString entry = read( autostartFile() ); !entry.isEmpty() && startsNothing( entryProgram( entry ) ) )
			{
				mended( "autostart", entryProgram( entry ) );
				( void )setAutostart( true );
			}
			if ( const QString entry = read( trayAutostartFile() ); !entry.isEmpty() && startsNothing( entryProgram( entry ) ) )
			{
				mended( "tray's autostart", entryProgram( entry ) );
				( void )setTrayAutostart( true );
			}
			if ( const QString entry = read( userEntry() ); ownMenuEntry( entry ) && startsNothing( entryProgram( entry ) ) )
			{
				mended( "menu entry", entryProgram( entry ) );
				( void )save( userEntry(), entryText() );
			}
			addMenuEntryOnce();
		}

	} // namespace

	bool menuEntryShown()
	{
		if ( QFile::exists( userEntry() ) )
		{
			return !hides( read( userEntry() ) );
		}
		return installedEntry();
	}

	bool setMenuEntry( bool shown, QString* error )
	{
		auto settings = applicationMemory();
		settings.setValue( QStringLiteral( "menu/decided" ), true );
		if ( shown )
		{
			return save( userEntry(), entryText(), error );
		}
		// An installed entry is hidden by one of the user's; otherwise there is only the user's to remove.
		if ( installedEntry() )
		{
			return save( userEntry(), QStringLiteral( "[Desktop Entry]\nType=Application\nName=Lexiglance\nNoDisplay=true\nHidden=true\n%1\n" ).arg( generated_key ), error );
		}
		if ( QFile::exists( userEntry() ) && !QFile::remove( userEntry() ) )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot remove %1" ).arg( userEntry() );
			}
			return false;
		}
		return true;
	}

	bool autostartEnabled()
	{
		return QFile::exists( autostartFile() );
	}

	bool setAutostart( bool enabled )
	{
		if ( !enabled )
		{
			return QFile::remove( autostartFile() ) || !QFile::exists( autostartFile() );
		}
		return save( autostartFile(), autostartText() );
	}

	bool trayAutostartEnabled()
	{
		return QFile::exists( trayAutostartFile() );
	}

	bool setTrayAutostart( bool enabled )
	{
		if ( !enabled )
		{
			return QFile::remove( trayAutostartFile() ) || !QFile::exists( trayAutostartFile() );
		}
		return save( trayAutostartFile(), trayAutostartText() );
	}

	void claim()
	{
		// Written again only when they start something else (or start it differently), so nothing changes on every start.
		if ( const QString entry = read( autostartFile() ); !entry.isEmpty() && entry != autostartText() )
		{
			claimed( "autostart", entryProgram( entry ) );
			( void )setAutostart( true );
		}
		if ( const QString entry = read( trayAutostartFile() ); !entry.isEmpty() && entry != trayAutostartText() )
		{
			claimed( "tray's autostart", entryProgram( entry ) );
			( void )setTrayAutostart( true );
		}
		if ( const QString entry = read( userEntry() ); ownMenuEntry( entry ) && entry != entryText() )
		{
			claimed( "menu entry", entryProgram( entry ) );
			( void )save( userEntry(), entryText() );
		}
		addMenuEntryOnce();
	}

#endif

} // namespace lexiglance::gui::desktop
