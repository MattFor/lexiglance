#include "DesktopEntry.h"

#include "Common.h"

#include <lexiglance/core/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace lexiglance::gui::desktop
{

#ifdef Q_OS_MACOS

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

	void maintainMenuEntry() {}

#elifdef Q_OS_WIN

	namespace
	{

		// A shortcut in this user's Start menu.
		QString shortcut()
		{
			return QStandardPaths::writableLocation( QStandardPaths::ApplicationsLocation ) + QStringLiteral( "/Lexiglance.lnk" );
		}

		QString program()
		{
			return QFileInfo( QCoreApplication::applicationFilePath() ).canonicalFilePath();
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

	void maintainMenuEntry()
	{
		auto settings = applicationMemory();
		// The shortcut follows the program to where it runs now.
		if ( QFile::exists( shortcut() ) && QFileInfo( QFileInfo( shortcut() ).symLinkTarget() ).canonicalFilePath() != program() )
		{
			( void )setMenuEntry( true );
		}
		if ( !settings.value( QStringLiteral( "menu/decided" ), false ).toBool() )
		{
			settings.setValue( QStringLiteral( "menu/decided" ), true );
			if ( !menuEntryShown() )
			{
				( void )setMenuEntry( true );
			}
		}
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

		// Entries installed elsewhere (a package), which a user entry of the same name overrides.
		bool installedEntry()
		{
			return std::ranges::any_of( QStandardPaths::locateAll( QStandardPaths::GenericDataLocation, "applications/" + entry_name ), []( const QString& path ) { return QFileInfo( path ) != QFileInfo( userEntry() ); } );
		}

		QString read( const QString& path )
		{
			QFile file( path );
			return file.open( QIODevice::ReadOnly ) ? QString::fromUtf8( file.readAll() ) : QString();
		}

		bool hides( const QString& entry )
		{
			return entry.contains( QStringLiteral( "\nNoDisplay=true" ) ) || entry.contains( QStringLiteral( "\nHidden=true" ) );
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

		bool write( const QString& text, QString* error )
		{
			QDir().mkpath( QFileInfo( userEntry() ).absolutePath() );
			QSaveFile file( userEntry() );
			if ( !file.open( QIODevice::WriteOnly ) || file.write( text.toUtf8() ) < 0 || !file.commit() )
			{
				if ( error != nullptr )
				{
					*error = QStringLiteral( "Cannot write %1" ).arg( userEntry() );
				}
				return false;
			}
			return true;
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
			return write( entryText(), error );
		}
		// An installed entry is hidden by one of the user's; otherwise there is only the user's to remove.
		if ( installedEntry() )
		{
			return write( QStringLiteral( "[Desktop Entry]\nType=Application\nName=Lexiglance\nNoDisplay=true\nHidden=true\n%1\n" ).arg( generated_key ), error );
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

	void maintainMenuEntry()
	{
		auto          settings = applicationMemory();
		const QString current  = read( userEntry() );
		// An entry this program wrote follows it to where it runs now.
		if ( current.contains( generated_key ) && !hides( current ) && current != entryText() )
		{
			( void )write( entryText(), nullptr );
		}
		if ( !settings.value( QStringLiteral( "menu/decided" ), false ).toBool() )
		{
			settings.setValue( QStringLiteral( "menu/decided" ), true );
			if ( !menuEntryShown() )
			{
				( void )setMenuEntry( true );
			}
		}
	}

#endif

} // namespace lexiglance::gui::desktop
