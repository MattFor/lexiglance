#include "HelpOverlay.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		// What to do, and where: short enough to take in at a glance. Page names link to the pages.
		QString helpText( const QString& trigger, const QString& link_colour, bool first )
		{
			const auto page = [&]( const char* name, const char* title ) {
				return QStringLiteral( "<a href=\"%1\" style=\"color: %2; text-decoration: none; font-weight: 600\">%3</a>" ).arg( QLatin1String( name ), link_colour, QLatin1String( title ) );
			};
			const QList<std::pair<QString, QString>> rows{
				{ first ? QStringLiteral( "Get a dictionary (do this first)" ) : QStringLiteral( "Get a dictionary" ), page( "dictionaries", "Dictionaries" ) + QStringLiteral( " → Get recommended dictionaries" ) },
				{ QStringLiteral( "Look up a word" ), QStringLiteral( "Hold <b>%1</b> and point at it, in any program" ).arg( trigger.toHtmlEscaped() ) },
				{ QStringLiteral( "In the popup" ), QStringLiteral( "Wheel: more or less text · Left‑click an entry: copy it · Middle‑click: hear it · Right‑click: select text" ) },
				{ QStringLiteral( "Games, videos, images" ), page( "scanning", "Scanning" ) + QStringLiteral( " → Download PaddleOCR" ) },
				{ QStringLiteral( "Other trigger keys" ), page( "scanning", "Scanning" ) + QStringLiteral( " → Trigger" ) },
				{ QStringLiteral( "The popup's look" ), page( "appearance", "Appearance" ) },
				{ QStringLiteral( "Type a word in" ), page( "search", "Search" ) },
				{ QStringLiteral( "Anki cards" ), page( "anki", "Anki" ) },
				{ QStringLiteral( "Quickly change a value" ), QStringLiteral( "Click the field, then turn the wheel over it to run through the values; <b>0</b> means automatic where the field says so" ) },
				{ QStringLiteral( "Start with the computer" ), page( "overview", "Overview" ) + QStringLiteral( " → Startup" ) },
				{ QStringLiteral( "Something does not work" ), page( "overview", "Overview" ) + QStringLiteral( " → Check health" ) },
			};
			QString html = QStringLiteral( "<table cellspacing=\"0\" cellpadding=\"4\">" );
			for ( const auto& [what, where] : rows )
			{
				html += QStringLiteral( "<tr><td style=\"padding-right: 18px\">%1</td><td>%2</td></tr>" ).arg( what, where );
			}
			return html + QStringLiteral( "</table>" );
		}

	} // namespace

	HelpOverlay::HelpOverlay( std::function<void( const QString& )> show_page, QWidget* parent ) :
		QWidget( parent ),
		show_page_( std::move( show_page ) ),
		card_( new QFrame( this ) ),
		text_( new QLabel() ),
		close_( new QPushButton( QStringLiteral( "Got it" ) ) )
	{
		hide();
		setFocusPolicy( Qt::StrongFocus );
		card_->setObjectName( QStringLiteral( "helpCard" ) );

		auto* layout = new QVBoxLayout( card_ );
		layout->setContentsMargins( 26, 20, 26, 18 );
		layout->setSpacing( 12 );
		auto* title      = new QLabel( QStringLiteral( "Help" ) );
		QFont title_font = title->font();
		title_font.setPointSizeF( title_font.pointSizeF() * 1.4 );
		title_font.setBold( true );
		title->setFont( title_font );
		layout->addWidget( title );
		text_->setTextFormat( Qt::RichText );
		text_->setWordWrap( true );
		layout->addWidget( text_ );

		auto* footer = new QHBoxLayout();
		footer->addStretch( 1 );
		close_->setProperty( "primary", true );
		footer->addWidget( close_ );
		layout->addLayout( footer );

		connect( close_, &QPushButton::clicked, this, [this] { hide(); } );
		connect( text_, &QLabel::linkActivated, this, [this]( const QString& page ) {
			hide();
			show_page_( page );
		} );
		parent->installEventFilter( this );
	}

	void HelpOverlay::open( const QString& trigger, bool first )
	{
		text_->setText( helpText( trigger, palette().color( QPalette::Link ).name(), first ) );
		place();
		show();
		raise();
		close_->setFocus();
	}

	void HelpOverlay::place()
	{
		setGeometry( parentWidget()->rect() );
		card_->ensurePolished();
		const int width  = std::min( 660, this->width() - 48 );
		const int wanted = card_->heightForWidth( width );
		const int height = std::min( wanted > 0 ? wanted : card_->sizeHint().height(), this->height() - 48 );
		card_->setGeometry( ( this->width() - width ) / 2, ( this->height() - height ) / 2, width, height );
	}

	bool HelpOverlay::eventFilter( QObject* watched, QEvent* event )
	{
		if ( watched == parentWidget() && event->type() == QEvent::Resize && isVisible() )
		{
			place();
		}
		return QWidget::eventFilter( watched, event );
	}

	void HelpOverlay::paintEvent( QPaintEvent* /*event*/ )
	{
		QPainter painter( this );
		painter.fillRect( rect(), QColor( 0, 0, 0, 110 ) );
	}

	void HelpOverlay::mousePressEvent( QMouseEvent* event )
	{
		// Clicks on the card come here too, as it takes none itself.
		if ( !card_->geometry().contains( event->position().toPoint() ) )
		{
			hide();
		}
	}

	void HelpOverlay::keyPressEvent( QKeyEvent* event )
	{
		if ( event->key() == Qt::Key_Escape )
		{
			hide();
			return;
		}
		QWidget::keyPressEvent( event );
	}

} // namespace lexiglance::gui
