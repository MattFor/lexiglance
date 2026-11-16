#include "DesktopEntry.h"

#include "Common.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
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
		// wrote, and which disappears when the AppImage ends). A bare name is looked up on the PATH.
		bool startsNothing( const QString& program )
		{
			if ( program.isEmpty() || program.contains( QStringLiteral( "/.mount_" ) ) )
			{
				return true;
			}
			return QFileInfo( program ).isRelative() ? QStandardPaths::findExecutable( program ).isEmpty() : !QFileInfo::exists( program );
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

		// The AppImage alone in a folder of its own (~/Applications, as an earlier update made it) while the user keeps
		// AppImages in another (~/applications): it moves there, the empty folder goes, and the entries follow it (claim).
		void moveAppImage()
		{
			const QString appimage = qEnvironmentVariable( "APPIMAGE" );
			if ( appimage.isEmpty() )
			{
				return;
			}
			const QString target = qs( paths::appImageDestination( ss( appimage ), ss( QDir::homePath() ) ).string() );
			// The running AppImage keeps its open file, so it can be moved while it runs.
			if ( target.isEmpty() || !QFile::rename( appimage, target ) )
			{
				if ( !target.isEmpty() )
				{
					log::warn( "entries: cannot move {} to {}", ss( appimage ), ss( target ) );
				}
				return;
			}
			( void )QDir().rmdir( QFileInfo( appimage ).absolutePath() );
			qputenv( "APPIMAGE", QFile::encodeName( target ) );
			log::info( "entries: moved {} to {}, beside the other AppImages", ss( appimage ), ss( target ) );
		}

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
		moveAppImage();
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
		// Marks entries this program wrote, with a checksum of the rest: one that no longer matches was edited by hand.
		// Up to 1.3.9 the value was "true".
		const QString generated_key = QStringLiteral( "X-Lexiglance-Generated=" );
		const QString categories    = QStringLiteral( "Categories=Utility;TextTools;Dictionary;\n" );

		QString userApplications()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/applications";
		}

		QString userEntry()
		{
			return userApplications() + "/" + entry_name;
		}

		QString autostartFile()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-daemon.desktop";
		}

		QString trayAutostartFile()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-tray.desktop";
		}

		const QRegularExpression hiding_line( QStringLiteral( "^(?:NoDisplay|Hidden)=true\\n?" ), QRegularExpression::MultilineOption );
		const QRegularExpression marker_line( QStringLiteral( "^X-Lexiglance-Generated=([^\\n]*)\\n?" ), QRegularExpression::MultilineOption );
		// An argument of an Exec line: quoted (with backslash escapes) or not.
		const QRegularExpression exec_argument( QStringLiteral( R"re("((?:[^"\\]|\\.)*)"|(\S+))re" ) );

		bool hides( const QString& entry )
		{
			return hiding_line.match( entry ).hasMatch();
		}

		bool generated( const QString& entry )
		{
			return marker_line.match( entry ).hasMatch();
		}

		// The program a desktop entry starts, from its (first) Exec line.
		QString entryProgram( const QString& entry )
		{
			static const QRegularExpression exec( QStringLiteral( "^Exec=(.*)$" ), QRegularExpression::MultilineOption );
			return commandProgram( exec.match( entry ).captured( 1 ) );
		}

		// A Lexiglance program, by its file name: the settings application, or an AppImage of it however it was renamed
		// (Lexiglance-x86_64_1a2b.AppImage by AppImageLauncher, lexiglance.appimage by Gear Lever).
		bool lexiglanceProgram( const QString& path )
		{
			static const QRegularExpression name( QStringLiteral( R"(^lexiglance$|^lexiglance(?:[-_. ][^/]*)?\.appimage$)" ), QRegularExpression::CaseInsensitiveOption );
			return name.match( QFileInfo( path ).fileName() ).hasMatch();
		}

		QString unescaped( QString argument )
		{
			static const QRegularExpression escape( QStringLiteral( R"(\\(.))" ) );
			return argument.replace( escape, QStringLiteral( "\\1" ) );
		}

		// The Lexiglance an entry starts, also through a wrapper (env, ime-launch ...): the first one its Exec lines name.
		QString startedLexiglance( const QString& entry )
		{
			static const QRegularExpression exec( QStringLiteral( "^Exec=(.*)$" ), QRegularExpression::MultilineOption );
			for ( const auto& line : exec.globalMatch( entry ) )
			{
				for ( const auto& argument : exec_argument.globalMatch( line.captured( 1 ) ) )
				{
					const QString value = argument.hasCaptured( 1 ) ? unescaped( argument.captured( 1 ) ) : argument.captured( 2 );
					if ( lexiglanceProgram( value ) )
					{
						return value;
					}
				}
			}
			return {};
		}

		// A menu entry of Lexiglance's, whatever its file is called and wherever it is: one this program wrote, one named
		// like the package's (and a Flatpak's), or any other that starts a Lexiglance.
		bool lexiglanceEntry( const QString& file_name, const QString& entry )
		{
			return file_name == entry_name || generated( entry ) || !startedLexiglance( entry ).isEmpty();
		}

		struct MenuEntry
		{
			QString path;
			QString relative; // below applications/, which makes its desktop file ID
			QString text;
		};

		// Lexiglance's menu entries: of each desktop file ID the one the menu goes by, the first in the XDG data
		// directories (the user's own before the system's), as the menu does.
		std::vector<MenuEntry> menuEntries()
		{
			std::vector<MenuEntry> found;
			QSet<QString>          ids;
			for ( const QString& location : QStandardPaths::standardLocations( QStandardPaths::GenericDataLocation ) )
			{
				const QDir   applications( location + "/applications" );
				QDirIterator files( applications.path(), { QStringLiteral( "*.desktop" ) }, QDir::Files, QDirIterator::Subdirectories );
				while ( files.hasNext() )
				{
					const QString path     = files.next();
					const QString relative = applications.relativeFilePath( path );
					if ( !ids.contains( QString( relative ).replace( '/', '-' ) ) )
					{
						ids.insert( QString( relative ).replace( '/', '-' ) );
						if ( QString text = read( path ); lexiglanceEntry( QFileInfo( path ).fileName(), text ) )
						{
							found.push_back( { path, relative, std::move( text ) } );
						}
					}
				}
			}
			return found;
		}

		// Entries this program may change: the user's own files. A package's, or a link into one (a Flatpak's export), it
		// leaves alone.
		bool changeable( const QString& path )
		{
			const QFileInfo info( path );
			return !info.isSymLink() && info.isWritable() && QFileInfo( info.absolutePath() ).isWritable();
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
			const QString path     = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/icons/hicolor/scalable/apps/lexiglance.svg";
			const QString resource = QStringLiteral( ":/lexiglance/lexiglance.svg" );
			// Written again when it is an earlier version's logo.
			if ( read( path ) != read( resource ) )
			{
				QDir().mkpath( QFileInfo( path ).absolutePath() );
				QFile::remove( path );
				QFile::copy( resource, path );
				QFile::setPermissions( path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther );
			}
			return QFile::exists( path ) ? path : QStringLiteral( "accessories-dictionary" );
		}

		// The entry this program writes for `target`, with `marker` as its marker line (none when empty).
		QString renderEntry( const QString& target, const QString& icon, const QString& marker )
		{
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
						   "%5"
						   "Keywords=dictionary;japanese;russian;ukrainian;korean;greek;yomitan;popup;translate;\n"
						   "StartupNotify=true\n"
						   "Actions=health;search;\n"
						   "%4"
						   "\n"
						   "[Desktop Action health]\n"
						   "Name=Check health\n"
						   "Exec=%1 --page overview\n"
						   "\n"
						   "[Desktop Action search]\n"
						   "Name=Search\n"
						   "Exec=%1 --page search\n"
			)
			        .arg( quoted( target ), target, icon, marker, categories );
		}

		QString withoutMarker( QString entry )
		{
			return entry.remove( marker_line );
		}

		QString checksum( const QString& entry )
		{
			return QString::fromLatin1( QCryptographicHash::hash( withoutMarker( entry ).toUtf8(), QCryptographicHash::Sha256 ).toHex().left( 16 ) );
		}

		QString entryText()
		{
			const QString icon = installIcon();
			return renderEntry( program(), icon, generated_key + checksum( renderEntry( program(), icon, {} ) ) + "\n" );
		}

		// Whether an entry this program wrote is still as it wrote it, so that a newer version may write it afresh. One
		// changed by hand keeps the changes: only the program it starts is ever changed in it.
		bool unedited( const QString& entry )
		{
			const QString marker = marker_line.match( entry ).captured( 1 );
			if ( marker != QStringLiteral( "true" ) )
			{
				return marker == checksum( entry );
			}
			// Up to 1.3.9, without a checksum: unedited when it is what 1.3.9 (or, with its categories, 1.3.8) wrote.
			const QString before = renderEntry( entryProgram( entry ), installIcon(), generated_key + "true\n" );
			return entry == before || entry == QString( before ).replace( categories, QStringLiteral( "Categories=Education;Languages;\n" ) );
		}

		// The entry with the Lexiglance it starts (in Exec and TryExec lines) replaced by `target`, and nothing else changed.
		QString repointed( const QString& entry, const QString& target )
		{
			QStringList lines = entry.split( '\n' );
			for ( QString& line : lines )
			{
				if ( line.startsWith( QStringLiteral( "TryExec=" ) ) && lexiglanceProgram( line.mid( 8 ) ) )
				{
					line = QStringLiteral( "TryExec=" ) + target;
				}
				else if ( line.startsWith( QStringLiteral( "Exec=" ) ) )
				{
					// From the last argument back, so that the earlier ones stay where they were matched.
					std::vector<QRegularExpressionMatch> arguments;
					for ( const auto& argument : exec_argument.globalMatch( line, 5 ) )
					{
						arguments.push_back( argument );
					}
					for ( auto argument = arguments.rbegin(); argument != arguments.rend(); ++argument )
					{
						if ( lexiglanceProgram( argument->hasCaptured( 1 ) ? unescaped( argument->captured( 1 ) ) : argument->captured( 2 ) ) )
						{
							line.replace( argument->capturedStart(), argument->capturedLength(), quoted( target ) );
						}
					}
				}
			}
			return lines.join( '\n' );
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

		// Points the menu entries at this copy. One this program wrote follows a claim; any other (AppImageLauncher's, the
		// user's own) is the user's, and changes only once the program it starts is gone, as do all on a mend. Of one this
		// program wrote and nobody changed the newest form is written; of any other, only the program it starts changes.
		void pointMenuEntries( bool claiming )
		{
			for ( const MenuEntry& entry : menuEntries() )
			{
				const QString started = startedLexiglance( entry.text );
				const bool    own     = generated( entry.text );
				if ( !changeable( entry.path ) || hides( entry.text ) || started.isEmpty() || !( startsNothing( started ) || ( claiming && own ) ) )
				{
					continue;
				}
				const QString wanted = own && unedited( entry.text ) ? entryText() : repointed( entry.text, program() );
				if ( wanted == entry.text )
				{
					continue;
				}
				if ( startsNothing( started ) )
				{
					mended( "menu entry", started );
				}
				else if ( !sameCopy( started, program() ) )
				{
					claimed( "menu entry", started );
				}
				( void )save( entry.path, wanted );
			}
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
			pointMenuEntries( false );
			addMenuEntryOnce();
		}

		bool failed( QString* error, const QString& message, const QString& path )
		{
			if ( error != nullptr )
			{
				*error = message.arg( path );
			}
			return false;
		}

	} // namespace

	bool menuEntryShown()
	{
		return std::ranges::any_of( menuEntries(), []( const MenuEntry& entry ) { return !hides( entry.text ); } );
	}

	bool setMenuEntry( bool shown, QString* error )
	{
		auto settings = applicationMemory();
		settings.setValue( QStringLiteral( "menu/decided" ), true );
		if ( shown )
		{
			// Back in by undoing what took it out: the entries this program hid others with go, the user's own entries
			// are shown again. A new entry only when that brings none back.
			for ( const MenuEntry& entry : menuEntries() )
			{
				if ( !hides( entry.text ) || !changeable( entry.path ) )
				{
					continue;
				}
				if ( generated( entry.text ) && entryProgram( entry.text ).isEmpty() ? !QFile::remove( entry.path ) : !save( entry.path, QString( entry.text ).remove( hiding_line ) ) )
				{
					return failed( error, QStringLiteral( "Cannot change %1" ), entry.path );
				}
			}
			return menuEntryShown() || save( userEntry(), entryText(), error );
		}
		// Out: an entry this program wrote goes, unless it was changed by hand; that one and the user's own are hidden in
		// place; an installed one is covered by a hiding entry of the user's with its desktop file ID. Again, as long as
		// that uncovers another of the same ID.
		for ( int round = 0; round < 8 && menuEntryShown(); ++round )
		{
			for ( const MenuEntry& entry : menuEntries() )
			{
				if ( hides( entry.text ) )
				{
					continue;
				}
				if ( !changeable( entry.path ) )
				{
					const QString cover = userApplications() + "/" + entry.relative;
					if ( QFile::exists( cover ) || !save( cover, QStringLiteral( "[Desktop Entry]\nType=Application\nName=Lexiglance\nNoDisplay=true\nHidden=true\n%1true\n" ).arg( generated_key ), error ) )
					{
						return failed( error, QStringLiteral( "Cannot hide %1" ), entry.path );
					}
				}
				else if ( generated( entry.text ) && unedited( entry.text ) ? !QFile::remove( entry.path ) : !save( entry.path, QString( entry.text ).replace( QStringLiteral( "[Desktop Entry]\n" ), QStringLiteral( "[Desktop Entry]\nNoDisplay=true\n" ) ) ) )
				{
					return failed( error, QStringLiteral( "Cannot change %1" ), entry.path );
				}
			}
		}
		return !menuEntryShown() || failed( error, QStringLiteral( "Cannot take %1 out of the menu" ), QStringLiteral( "Lexiglance" ) );
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
		pointMenuEntries( true );
		addMenuEntryOnce();
	}

#endif

} // namespace lexiglance::gui::desktop
