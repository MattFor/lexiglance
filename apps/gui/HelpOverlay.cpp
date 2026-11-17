#include "HelpOverlay.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		// What to do, and where: short enough to take in at a glance. Page names link to the pages.
		QString helpText( const QString& trigger, const QString& sentence, const QString& link_colour, bool first )
		{
			const auto page = [&]( const char* name, const char* title ) {
				return QStringLiteral( "<a href=\"%1\" style=\"color: %2; text-decoration: none; font-weight: 600\">%3</a>" ).arg( QLatin1String( name ), link_colour, QLatin1String( title ) );
			};
			const QList<std::pair<QString, QString>> rows{
				{ first ? QStringLiteral( "Get a dictionary (do this first)" ) : QStringLiteral( "Get a dictionary" ), page( "dictionaries", "Dictionaries" ) + QStringLiteral( " → Get recommended dictionaries" ) },
				{ QStringLiteral( "Look up a word" ), QStringLiteral( "Hold <b>%1</b> and point at it, in any program" ).arg( trigger.toHtmlEscaped() ) },
				{ QStringLiteral( "Select more characters" ), QStringLiteral( "Keep holding it and turn the scroll wheel: up selects more, down fewer" ) },
				{ QStringLiteral( "Translate a sentence" ),
				  sentence.isEmpty() ? QStringLiteral( "Choose a sentence key on " ) + page( "translation", "Translation" )
				                     : QStringLiteral( "Hold <b>%1</b> as well to see the sentence, or the characters you selected, in English. It stays when you let go; press it again to hide it · Models: " ).arg( sentence.toHtmlEscaped() ) +
				                               page( "translation", "Translation" ) },
				{ QStringLiteral( "Use the popup" ),
				  QStringLiteral( "It closes when you let go of the keys, unless the pointer is on it, so move onto it first. Then: scroll for more entries · left‑click an entry to copy it · middle‑click to hear it · right‑click to select text" ) },
				{ QStringLiteral( "Games, videos, images" ), page( "scanning", "Scanning" ) + QStringLiteral( " → Download PaddleOCR" ) },
				{ QStringLiteral( "Change the trigger" ), page( "scanning", "Scanning" ) + QStringLiteral( " → Trigger" ) },
				{ QStringLiteral( "Change how the popup looks" ), page( "appearance", "Appearance" ) },
				{ QStringLiteral( "Type a word in, or find one looked up lately" ), page( "search", "Search" ) },
				{ QStringLiteral( "Make Anki cards" ), page( "anki", "Anki" ) },
				{ QStringLiteral( "Change a number quickly" ), QStringLiteral( "Click the field, then turn the scroll wheel over it. <b>0</b> means automatic where the field says so" ) },
				{ QStringLiteral( "Start with the computer" ), page( "overview", "Overview" ) + QStringLiteral( " → Startup" ) },
				{ QStringLiteral( "Take the settings to another computer" ), page( "overview", "Overview" ) + QStringLiteral( " → Settings → Export, then Import there" ) },
				{ QStringLiteral( "Something doesn't work" ), page( "overview", "Overview" ) + QStringLiteral( " → Check health" ) },
				{ QStringLiteral( "Tell us about a problem" ), page( "report", "Report a problem" ) },
			};
			QString html = QStringLiteral( "<table cellspacing=\"0\" cellpadding=\"4\">" );
			for ( const auto& [what, where] : rows )
			{
				html += QStringLiteral( "<tr><td style=\"padding-right: 18px\">%1</td><td>%2</td></tr>" ).arg( what, where );
			}
			return html + QStringLiteral( "</table>" );
		}

	} // namespace

	HelpOverlay::HelpOverlay( std::function<void( const QString& )> show_page, std::function<void()> simulate_setup, std::function<void()> simulate_update, QWidget* parent ) :
		CardOverlay( 660, parent ),
		show_page_( std::move( show_page ) ),
		simulate_setup_( std::move( simulate_setup ) ),
		simulate_update_( std::move( simulate_update ) ),
		text_( new QLabel() ),
		close_( new QPushButton( QStringLiteral( "Got it" ) ) ),
		simulate_( new QPushButton( QStringLiteral( "Run setup again" ) ) ),
		simulate_update_button_( new QPushButton( QStringLiteral( "What's new" ) ) )
	{
		auto* layout = cardLayout();
		layout->addWidget( cardTitle( QStringLiteral( "Help" ) ) );
		text_->setTextFormat( Qt::RichText );
		text_->setWordWrap( true );
		layout->addWidget( text_ );

		auto* footer = new QHBoxLayout();
		simulate_->setToolTip( QStringLiteral( "Opens the first-run setup again: languages, downloads and the sentence to try. Dictionaries you have stay." ) );
		simulate_->setVisible( static_cast<bool>( simulate_setup_ ) );
		footer->addWidget( simulate_ );
		simulate_update_button_->setToolTip( QStringLiteral( "What changed in this version" ) );
		simulate_update_button_->setVisible( static_cast<bool>( simulate_update_ ) );
		footer->addWidget( simulate_update_button_ );
		footer->addStretch( 1 );
		close_->setProperty( "primary", true );
		footer->addWidget( close_ );
		layout->addLayout( footer );

		connect( close_, &QPushButton::clicked, this, [this] { hide(); } );
		connect( simulate_, &QPushButton::clicked, this, [this] {
			hide();
			if ( simulate_setup_ )
			{
				simulate_setup_();
			}
		} );
		connect( simulate_update_button_, &QPushButton::clicked, this, [this] {
			hide();
			if ( simulate_update_ )
			{
				simulate_update_();
			}
		} );
		connect( text_, &QLabel::linkActivated, this, [this]( const QString& page ) {
			hide();
			show_page_( page );
		} );
	}

	void HelpOverlay::open( const QString& trigger, const QString& sentence, bool first )
	{
		text_->setText( helpText( trigger, sentence, palette().color( QPalette::Link ).name(), first ) );
		popUp();
		close_->setFocus();
	}

} // namespace lexiglance::gui
