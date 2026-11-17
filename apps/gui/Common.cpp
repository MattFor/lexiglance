#include "Common.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Paths.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>

#ifdef Q_OS_MACOS
	#include <objc/message.h>
	#include <objc/runtime.h>

// CoreGraphics' (CGEvent.h, CGWindow.h); its headers are not C++26-clean.
extern "C" bool CGPreflightListenEventAccess( void );
extern "C" bool CGPreflightScreenCaptureAccess( void );
#endif

namespace lexiglance::gui
{

	QString formatBytes( std::uint64_t bytes )
	{
		const auto value = static_cast<double>( bytes );
		if ( value >= 1024.0 * 1024.0 * 1024.0 )
		{
			return QString::number( value / ( 1024.0 * 1024.0 * 1024.0 ), 'f', 1 ) + " GiB";
		}
		if ( value >= 1024.0 * 1024.0 )
		{
			return QString::number( value / ( 1024.0 * 1024.0 ), 'f', 1 ) + " MiB";
		}
		return QString::number( value / 1024.0, 'f', 0 ) + " KiB";
	}

	QString chordText( const std::vector<std::string>& keys )
	{
		QStringList parts;
		for ( const auto& key : keys )
		{
			parts << qs( config::displayName( key ) );
		}
		return parts.isEmpty() ? QStringLiteral( "(none)" ) : parts.join( QStringLiteral( " + " ) );
	}

	QString withLinks( QString escaped )
	{
		static const QRegularExpression address( QStringLiteral( R"((https?://[^\s<]*[^\s<.,;:!?)]))" ) );
		return escaped.replace( address, QStringLiteral( R"(<a href="\1">\1</a>)" ) );
	}

	QString shortTitle( const QString& title )
	{
		const auto bracket = title.lastIndexOf( QStringLiteral( " [" ) );
		return bracket > 0 && title.endsWith( ']' ) ? title.left( bracket ) : title;
	}

	QString daemonExecutable()
	{
#ifdef Q_OS_WIN
		const QString name = QStringLiteral( "lexiglanced.exe" );
#else
		const QString name = QStringLiteral( "lexiglanced" );
#endif
		const QString here = QCoreApplication::applicationDirPath();
		// Beside it, in a build tree, or Lexiglance.app's helper app (see process::sibling).
		for ( const QString& candidate : { here + "/" + name, here + "/../daemon/" + name, here + "/../Helpers/lexiglanced.app/Contents/MacOS/" + name } )
		{
			if ( QFileInfo( candidate ).isExecutable() )
			{
				return QFileInfo( candidate ).canonicalFilePath();
			}
		}
		const QString found = QStandardPaths::findExecutable( QStringLiteral( "lexiglanced" ) );
		return found.isEmpty() ? name : found;
	}

	bool confirm( QWidget* parent, const QString& title, const QString& text, const QString& action )
	{
		QMessageBox  box( QMessageBox::Question, title, text, QMessageBox::Cancel, parent );
		QPushButton* accept = box.addButton( action, QMessageBox::AcceptRole );
		box.setDefaultButton( QMessageBox::Cancel );
		box.exec();
		return box.clickedButton() == accept;
	}

	void showProgress( QProgressBar* bar, qint64 received, qint64 total )
	{
		bar->show();
		if ( total <= 0 )
		{
			bar->setRange( 0, 0 );
			return;
		}
		bar->setRange( 0, 1000 );
		bar->setValue( static_cast<int>( std::min( received, total ) * 1000 / total ) );
		bar->setFormat( QStringLiteral( "%1 of %2" ).arg( formatBytes( static_cast<std::uint64_t>( received ) ), formatBytes( static_cast<std::uint64_t>( total ) ) ) );
	}

	bool olderVersion( const QString& a, const QString& b )
	{
		const auto number = []( const QString& text ) { return QVersionNumber::fromString( text.startsWith( 'v' ) ? text.mid( 1 ) : text ); };
		const auto first  = number( a );
		const auto second = number( b );
		return !first.isNull() && !second.isNull() && first < second;
	}

	QSettings applicationMemory()
	{
		return { qs( ( paths::configDir() / "settings-application.ini" ).string() ), QSettings::IniFormat };
	}

	void showInDock( [[maybe_unused]] bool shown )
	{
#ifdef Q_OS_MACOS
		// [NSApp setActivationPolicy:] with Regular (0) or Accessory (1): Qt has no call for it.
		id app = reinterpret_cast<id ( * )( Class, SEL )>( objc_msgSend )( objc_getClass( "NSApplication" ), sel_registerName( "sharedApplication" ) );
		reinterpret_cast<void ( * )( id, SEL, long )>( objc_msgSend )( app, sel_registerName( "setActivationPolicy:" ), shown ? 0 : 1 );
#endif
	}

	bool openPrivacySettings( const QString& action )
	{
		static const QHash<QString, QString> panes{
			{ QStringLiteral( "open-privacy-accessibility" ), QStringLiteral( "Accessibility" ) },
			{ QStringLiteral( "open-privacy-input-monitoring" ), QStringLiteral( "ListenEvent" ) },
			{ QStringLiteral( "open-privacy-screen-recording" ), QStringLiteral( "ScreenCapture" ) },
		};
		const QString pane = panes.value( action );
		return !pane.isEmpty() && QDesktopServices::openUrl( QUrl( QStringLiteral( "x-apple.systempreferences:com.apple.preference.security?Privacy_" ) + pane ) );
	}

	bool privacyAllowed( [[maybe_unused]] std::string_view fix )
	{
#ifdef Q_OS_MACOS
		if ( fix == "open-privacy-input-monitoring" )
		{
			return CGPreflightListenEventAccess();
		}
		if ( fix == "open-privacy-screen-recording" )
		{
			return CGPreflightScreenCaptureAccess();
		}
#endif
		return false;
	}

	QString diagnosisZipPath()
	{
		return qs( ( paths::cacheDir() / "diagnosis" / "lexiglance-diagnosis.zip" ).string() );
	}

	void clearDiagnosisZip()
	{
		const QString zip = diagnosisZipPath();
		QFile::remove( zip );
		QDir().rmdir( QFileInfo( zip ).absolutePath() );
	}

} // namespace lexiglance::gui
