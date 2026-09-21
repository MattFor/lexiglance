#include "ChangesOverlay.h"

#include <QFile>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		// The version a section of CHANGELOG.md is for, and what it says: the one under "## [version]", or the first
		// when there is none for `version` (a dev build ahead of the changelog).
		std::pair<QString, QString> changes( const QString& version )
		{
			QFile file( QStringLiteral( ":/lexiglance/CHANGELOG.md" ) );
			if ( !file.open( QIODevice::ReadOnly ) )
			{
				return {};
			}
			const QString                   text = QString::fromUtf8( file.readAll() );
			static const QRegularExpression heading( QStringLiteral( "^## \\[([^\\]]+)\\].*$" ), QRegularExpression::MultilineOption );
			QRegularExpressionMatch         chosen;
			for ( auto matches = heading.globalMatch( text ); matches.hasNext(); )
			{
				const auto match = matches.next();
				if ( !chosen.hasMatch() || match.captured( 1 ) == version )
				{
					chosen = match;
				}
				if ( match.captured( 1 ) == version )
				{
					break;
				}
			}
			if ( !chosen.hasMatch() )
			{
				return {};
			}
			const qsizetype begin = chosen.capturedEnd();
			const qsizetype end   = text.indexOf( QStringLiteral( "\n## " ), begin );
			return { chosen.captured( 1 ), text.mid( begin, end < 0 ? -1 : end - begin ).trimmed() };
		}

	} // namespace

	ChangesOverlay::ChangesOverlay( QWidget* parent ) :
		CardOverlay( 620, parent ),
		title_( new QLabel() ),
		text_( new QLabel() ),
		close_( new QPushButton( QStringLiteral( "Okie dokie" ) ) )
	{
		auto* layout = new QVBoxLayout( card() );
		layout->setContentsMargins( 26, 20, 26, 18 );
		layout->setSpacing( 12 );
		QFont title_font = title_->font();
		title_font.setPointSizeF( title_font.pointSizeF() * 1.4 );
		title_font.setBold( true );
		title_->setFont( title_font );
		layout->addWidget( title_ );
		text_->setTextFormat( Qt::MarkdownText );
		text_->setWordWrap( true );
		text_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		layout->addWidget( text_ );

		auto* footer = new QHBoxLayout();
		footer->addStretch( 1 );
		close_->setProperty( "primary", true );
		footer->addWidget( close_ );
		layout->addLayout( footer );

		connect( close_, &QPushButton::clicked, this, [this] { hide(); } );
	}

	void ChangesOverlay::open( const QString& version )
	{
		const auto [shown, text] = changes( version );
		title_->setText( shown.isEmpty() ? QStringLiteral( "What's new" ) : QStringLiteral( "What's new in Lexiglance %1" ).arg( shown ) );
		text_->setText( text.isEmpty() ? QStringLiteral( "Lexiglance was updated to %1." ).arg( version ) : text );
		popUp();
		close_->setFocus();
	}

} // namespace lexiglance::gui
