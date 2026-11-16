#include "ChangesOverlay.h"

#include "Common.h"

#include <QFile>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		struct Section
		{
			QString version;
			QString text;
		};

		// The sections of CHANGELOG.md, newest first as it has them: the version under "## [version]", and what it says.
		std::vector<Section> sections()
		{
			QFile file( QStringLiteral( ":/lexiglance/CHANGELOG.md" ) );
			if ( !file.open( QIODevice::ReadOnly ) )
			{
				return {};
			}
			const QString                   text = QString::fromUtf8( file.readAll() );
			static const QRegularExpression heading( QStringLiteral( "^## \\[([^\\]]+)\\].*$" ), QRegularExpression::MultilineOption );
			std::vector<Section>            found;
			for ( const auto& match : heading.globalMatch( text ) )
			{
				const qsizetype end = text.indexOf( QStringLiteral( "\n## " ), match.capturedEnd() );
				found.push_back( { match.captured( 1 ), text.mid( match.capturedEnd(), end < 0 ? -1 : end - match.capturedEnd() ).trimmed() } );
			}
			return found;
		}

	} // namespace

	ChangesOverlay::ChangesOverlay( QWidget* parent ) :
		CardOverlay( 620, parent ),
		title_( new QLabel() ),
		text_( new QLabel() ),
		scroll_( new QScrollArea() ),
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
		scroll_->setWidget( text_ );
		scroll_->setWidgetResizable( true );
		scroll_->setFrameShape( QFrame::NoFrame );
		scroll_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		// The card's background through it.
		scroll_->viewport()->setAutoFillBackground( false );
		text_->setAutoFillBackground( false );
		layout->addWidget( scroll_ );

		auto* footer = new QHBoxLayout();
		footer->addStretch( 1 );
		close_->setProperty( "primary", true );
		footer->addWidget( close_ );
		layout->addLayout( footer );

		connect( close_, &QPushButton::clicked, this, [this] { hide(); } );
	}

	void ChangesOverlay::open( const QString& version, const QString& since )
	{
		const std::vector<Section> all = sections();
		std::vector<Section>       shown;
		if ( !since.isEmpty() )
		{
			std::ranges::copy_if( all, std::back_inserter( shown ), [&]( const Section& section ) { return olderVersion( since, section.version ) && !olderVersion( version, section.version ); } );
		}
		if ( shown.empty() )
		{
			// A dev build ahead of the changelog has none of its own: the newest there is.
			const auto own = std::ranges::find( all, version, &Section::version );
			if ( own != all.end() || !all.empty() )
			{
				shown.push_back( own != all.end() ? *own : all.front() );
			}
		}
		QString text;
		for ( const Section& section : shown )
		{
			text += shown.size() > 1 ? QStringLiteral( "## %1\n\n%2\n\n" ).arg( section.version, section.text ) : section.text;
		}
		if ( shown.size() > 1 )
		{
			title_->setText( QStringLiteral( "What's new since Lexiglance %1" ).arg( since ) );
		}
		else
		{
			title_->setText( shown.empty() ? QStringLiteral( "What's new" ) : QStringLiteral( "What's new in Lexiglance %1" ).arg( shown.front().version ) );
		}
		text_->setText( text.trimmed().isEmpty() ? QStringLiteral( "Lexiglance was updated to %1." ).arg( version ) : text.trimmed() );
		// As tall as the changes, up to most of the window; past that it scrolls, from the newest.
		const int text_width = std::min( 620, parentWidget()->width() - 48 ) - 52 - scroll_->verticalScrollBar()->sizeHint().width();
		scroll_->setMinimumHeight( std::min( text_->heightForWidth( text_width ), parentWidget()->height() * 3 / 5 ) );
		scroll_->verticalScrollBar()->setValue( 0 );
		popUp();
		close_->setFocus();
	}

} // namespace lexiglance::gui
