#include "ChangesOverlay.h"

#include "Common.h"

#include <lexiglance/core/Version.h>

#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextList>
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

		// Markdown's headings come out poster-sized: versions a step larger than the text, their sections bold, and some
		// air between the points.
		void tidy( QTextDocument* document )
		{
			QTextCursor cursor( document );
			for ( QTextBlock block = document->begin(); block.isValid(); block = block.next() )
			{
				QTextBlockFormat format = block.blockFormat();
				if ( const int level = format.headingLevel(); level > 0 )
				{
					QTextCharFormat chars;
					chars.setProperty( QTextFormat::FontSizeAdjustment, level <= 2 ? 1 : 0 );
					chars.setFontWeight( QFont::Bold );
					cursor.setPosition( block.position() );
					cursor.setPosition( block.position() + block.length() - 1, QTextCursor::KeepAnchor );
					cursor.mergeCharFormat( chars );
					format.setTopMargin( block == document->begin() ? 0 : ( level <= 2 ? 18 : 10 ) );
					format.setBottomMargin( 4 );
				}
				else if ( block.textList() != nullptr )
				{
					format.setBottomMargin( 6 );
				}
				cursor.setPosition( block.position() );
				cursor.setBlockFormat( format );
			}
		}

	} // namespace

	ChangesOverlay::ChangesOverlay( QWidget* parent ) :
		CardOverlay( 620, parent ),
		title_( cardTitle() ),
		text_( new QTextBrowser() ),
		close_( new QPushButton( QStringLiteral( "Got it" ) ) )
	{
		auto* layout = cardLayout();
		layout->addWidget( title_ );
		// On the card itself, not in a field.
		text_->setStyleSheet( QStringLiteral( "QTextBrowser { background: transparent; border: none; padding: 0; }" ) );
		text_->setFrameShape( QFrame::NoFrame );
		text_->viewport()->setAutoFillBackground( false );
		text_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		text_->setFocusPolicy( Qt::NoFocus );
		text_->setOpenLinks( false );
		layout->addWidget( text_ );

		auto* footer = new QHBoxLayout();
		footer->addStretch( 1 );
		close_->setProperty( "primary", true );
		footer->addWidget( close_ );
		layout->addLayout( footer );

		connect( close_, &QPushButton::clicked, this, [this] { hide(); } );
		// The changelog links its docs by their place in the repository.
		connect( text_, &QTextBrowser::anchorClicked, this, []( const QUrl& link ) {
			QDesktopServices::openUrl( link.isRelative() ? QUrl( qs( project_url ) + QStringLiteral( "/blob/master/" ) ).resolved( link ) : link );
		} );
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
		text_->setMarkdown( text.trimmed().isEmpty() ? QStringLiteral( "Lexiglance is now version %1." ).arg( version ) : text.trimmed() );
		tidy( text_->document() );
		// As tall as the changes, up to most of the window; past that it scrolls, from the newest.
		const int text_width = std::min( 620, parentWidget()->width() - 48 ) - 56 - text_->verticalScrollBar()->sizeHint().width();
		text_->document()->setTextWidth( text_width );
		const int wanted = static_cast<int>( text_->document()->size().height() ) + 4;
		text_->setFixedHeight( std::min( wanted, parentWidget()->height() * 3 / 5 ) );
		text_->verticalScrollBar()->setValue( 0 );
		popUp();
		close_->setFocus();
	}

} // namespace lexiglance::gui
