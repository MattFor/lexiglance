#include "Common.h"

#include <lexiglance/core/Paths.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVersionNumber>

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
			parts << qs( key );
		}
		return parts.isEmpty() ? QStringLiteral( "(none)" ) : parts.join( QStringLiteral( " + " ) );
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
		for ( const QString& candidate : { here + "/" + name, here + "/../daemon/" + name } )
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

} // namespace lexiglance::gui
