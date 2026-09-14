#include "Common.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>

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

} // namespace lexiglance::gui
