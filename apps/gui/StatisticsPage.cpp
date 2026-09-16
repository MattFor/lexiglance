#include "StatisticsPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/language/Language.h>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		// The captions of the figures across the top, in the order show() fills them.
		constexpr std::array<const char*, 8> captions{ "Words looked up", "With an entry", "Different words", "Days used", "Popups shown", "Pronunciations played", "Anki notes added", "Characters read" };

		QFrame* tile( QLabel* value, const QString& caption )
		{
			auto* frame = new QFrame();
			frame->setObjectName( QStringLiteral( "tile" ) );
			auto* layout = new QVBoxLayout( frame );
			layout->setContentsMargins( 12, 8, 12, 8 );
			layout->setSpacing( 2 );
			value->setObjectName( QStringLiteral( "tileValue" ) );
			value->setTextInteractionFlags( Qt::TextSelectableByMouse );
			auto* label = new QLabel( caption );
			label->setObjectName( QStringLiteral( "tileCaption" ) );
			label->setWordWrap( true );
			layout->addWidget( value );
			layout->addWidget( label );
			return frame;
		}

		QLabel* heading( const QString& text )
		{
			auto* label = new QLabel( text );
			QFont font  = label->font();
			font.setPointSizeF( font.pointSizeF() * 1.9 );
			font.setBold( true );
			label->setFont( font );
			return label;
		}

		QString number( std::int64_t value )
		{
			return QLocale().toString( static_cast<qlonglong>( value ) );
		}

		// One row of a chart: what it is, how often, and a bar as long as its share of the largest.
		QString bar( const QString& label, std::int64_t count, std::int64_t largest, const QString& colour )
		{
			const int share = largest > 0 ? std::clamp( static_cast<int>( ( count * 100 ) / largest ), 2, 100 ) : 2;
			return QStringLiteral( "<tr><td style=\"padding-right: 14px\">%1</td><td align=\"right\" style=\"padding-right: 10px\">%2</td>"
			                       "<td width=\"55%\"><table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td width=\"%3%\" bgcolor=\"%4\">&nbsp;</td><td></td></tr></table></td></tr>" )
			        .arg( label.toHtmlEscaped(), number( count ) )
			        .arg( share )
			        .arg( colour );
		}

		// A chart of the entries of `items`, each an object with a `key` and a count, largest first.
		QString chart( const json::Value& items, const char* key, const QString& colour, int limit )
		{
			std::int64_t largest = 0;
			for ( const json::Value& item : items.items() )
			{
				largest = std::max( largest, item["count"].asInt() );
			}
			QString rows;
			int     shown = 0;
			for ( const json::Value& item : items.items() )
			{
				if ( shown++ >= limit )
				{
					break;
				}
				rows += bar( qs( item[key].asString() ), item["count"].asInt(), largest, colour );
			}
			return rows.isEmpty() ? QString() : QStringLiteral( "<table width=\"100%\" cellspacing=\"2\">%1</table>" ).arg( rows );
		}

		// Languages are counted by code; they are shown by name.
		QString languageName( const QString& code )
		{
			const lang::Language* language = lang::findLanguage( ss( code ) );
			return language != nullptr ? qs( language->name() ) : code;
		}

		// How the capture that read the text is called in the daemon, in words.
		QString sourceName( const QString& source )
		{
			if ( source.startsWith( QStringLiteral( "ui-automation" ) ) || source.startsWith( QStringLiteral( "at-spi" ) ) )
			{
				return QStringLiteral( "Text from applications (%1)" ).arg( source );
			}
			if ( source.startsWith( QStringLiteral( "ocr" ) ) )
			{
				return QStringLiteral( "Read from the screen (%1)" ).arg( source );
			}
			if ( source.startsWith( QStringLiteral( "selection" ) ) || source.startsWith( QStringLiteral( "clipboard" ) ) )
			{
				return QStringLiteral( "Selected or copied text" );
			}
			return source;
		}

	} // namespace

	StatisticsPage::StatisticsPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		detail_( new QTextBrowser() ),
		counting_( new QCheckBox( QStringLiteral( "Count what I look up" ) ) ),
		note_( new QLabel() ),
		poll_( new QTimer( this ) )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 28, 24, 28, 20 );
		layout->setSpacing( 14 );
		layout->addWidget( heading( QStringLiteral( "Statistics" ) ) );
		note_->setWordWrap( true );
		note_->setEnabled( false );
		layout->addWidget( note_ );

		// Two rows of four figures, the same tiles the overview uses.
		auto* grid = new QGridLayout();
		grid->setSpacing( 10 );
		for ( std::size_t i = 0; i < figures_.size(); ++i )
		{
			figures_[i] = new QLabel( QStringLiteral( "–" ) );
			grid->addWidget( tile( figures_[i], QString::fromLatin1( captions[i] ) ), static_cast<int>( i / 4 ), static_cast<int>( i % 4 ) );
		}
		layout->addLayout( grid );

		detail_->setOpenLinks( false );
		detail_->setFrameShape( QFrame::NoFrame );
		layout->addWidget( detail_, 1 );

		auto* row = new QHBoxLayout();
		counting_->setToolTip( QStringLiteral( "The tally is kept in this computer's state directory and is never sent anywhere. Turning this off stops counting; what was counted stays until it is forgotten." ) );
		row->addWidget( counting_, 1 );
		auto* forget = new QPushButton( QStringLiteral( "Forget everything..." ) );
		row->addWidget( forget );
		layout->addLayout( row );

		connect( counting_, &QCheckBox::toggled, this, [this]( bool on ) {
			settings().config().statistics = on;
			settings().commit();
			update();
		} );
		connect( forget, &QPushButton::clicked, this, [this] {
			if ( !confirm( this, QStringLiteral( "Forget the statistics" ), QStringLiteral( "Everything counted so far is forgotten: the words, the languages, the days. The dictionaries and settings are untouched." ), QStringLiteral( "Forget" ) ) )
			{
				return;
			}
			client().call( "stats.reset", "{}", [this]( const json::Value*, const QString& ) { update(); } );
		} );

		// While the page is open the figures follow along, so a lookup made next to this window shows up here.
		poll_->setInterval( 5000 );
		connect( poll_, &QTimer::timeout, this, [this] {
			if ( isVisible() )
			{
				update();
			}
		} );
		poll_->start();
	}

	void StatisticsPage::activated()
	{
		update();
	}

	void StatisticsPage::refresh()
	{
		const QSignalBlocker blocker( counting_ );
		counting_->setChecked( settings().config().statistics );
	}

	void StatisticsPage::update()
	{
		client().call( "stats", "{}", [this]( const json::Value* stats, const QString& error ) {
			if ( stats == nullptr )
			{
				// A daemon from before this page knows no such request; it counts nothing until it is restarted.
				const bool older = error.contains( QStringLiteral( "unknown method" ) );
				if ( older )
				{
					note_->setText( QStringLiteral( "The running daemon is older than this window and counts nothing yet. Restart Lexiglance (Overview → Restart) to start the tally." ) );
				}
				else if ( error.isEmpty() )
				{
					note_->setText( QStringLiteral( "Lexiglance is not running, so there is nothing to count with." ) );
				}
				else
				{
					note_->setText( error );
				}
				return;
			}
			show( *stats );
		} );
	}

	void StatisticsPage::show( const json::Value& stats )
	{
		const auto                        lookups = stats["lookups"].asInt();
		const auto                        found   = stats["found"].asInt();
		const std::array<std::int64_t, 8> values{ lookups, found, stats["distinct_words"].asInt(), stats["days_used"].asInt(), stats["popups"].asInt(), stats["audio"].asInt(), stats["anki"].asInt(), stats["characters"].asInt() };
		for ( std::size_t i = 0; i < figures_.size(); ++i )
		{
			figures_[i]->setText( number( values[i] ) );
		}

		const QString since = qs( stats["first_day"].asString() );
		QString       summary;
		if ( lookups > 0 )
		{
			summary = QStringLiteral( "Since %1: %2 words looked up over %3, %4 of them found (%5%), in %6 µs each." )
			                  .arg( since.isEmpty() ? QStringLiteral( "the first lookup" ) : since, number( lookups ) )
			                  .arg( stats["days_used"].asInt() == 1 ? QStringLiteral( "one day" ) : QStringLiteral( "%1 days" ).arg( stats["days_used"].asInt() ) )
			                  .arg( number( found ) )
			                  .arg( lookups > 0 ? ( found * 100 ) / lookups : 0 )
			                  .arg( stats["average_lookup_us"].asDouble(), 0, 'f', 1 );
		}
		else
		{
			summary = QStringLiteral( "Nothing has been looked up yet. Hold the trigger over a word and this page fills itself." );
		}
		if ( !stats["enabled"].asBool() )
		{
			summary += QStringLiteral( " Counting is off, so these figures stand still." );
		}
		note_->setText( summary );

		const QString muted = palette().color( QPalette::PlaceholderText ).name();
		QString       html;
		const auto    section = [&]( const QString& title, const QString& body, const QString& empty ) {
			html += QStringLiteral( "<h3>%1</h3>" ).arg( title );
			html += body.isEmpty() ? QStringLiteral( "<p style=\"color: %1\">%2</p>" ).arg( muted, empty ) : body;
		};

		section( QStringLiteral( "Words you look up most" ), chart( stats["words"], "word", QStringLiteral( "#5b8def" ), 20 ), QStringLiteral( "No word has been found twice yet." ) );

		{
			std::int64_t largest = 0;
			for ( const json::Value& item : stats["languages"].items() )
			{
				largest = std::max( largest, item["count"].asInt() );
			}
			QString rows;
			for ( const json::Value& item : stats["languages"].items() )
			{
				rows += bar( languageName( qs( item["language"].asString() ) ), item["count"].asInt(), largest, QStringLiteral( "#3fa45b" ) );
			}
			section( QStringLiteral( "Languages" ), rows.isEmpty() ? QString() : QStringLiteral( "<table width=\"100%\" cellspacing=\"2\">%1</table>" ).arg( rows ), QStringLiteral( "Nothing has been found in any language yet." ) );
		}

		// Where the text came from, with the daemon's own names put into words.
		{
			std::int64_t largest = 0;
			for ( const json::Value& item : stats["sources"].items() )
			{
				largest = std::max( largest, item["count"].asInt() );
			}
			QString rows;
			for ( const json::Value& item : stats["sources"].items() )
			{
				rows += bar( sourceName( qs( item["source"].asString() ) ), item["count"].asInt(), largest, QStringLiteral( "#d19a1f" ) );
			}
			section( QStringLiteral( "How the text was read" ), rows.isEmpty() ? QString() : QStringLiteral( "<table width=\"100%\" cellspacing=\"2\">%1</table>" ).arg( rows ), QStringLiteral( "Nothing has been read from the screen yet." ) );
		}

		// The last two weeks, oldest first, so a run of days reads left to right.
		{
			std::vector<std::pair<QString, std::int64_t>> days;
			for ( const json::Value& item : stats["days"].items() )
			{
				days.emplace_back( qs( item["day"].asString() ), item["count"].asInt() );
			}
			if ( days.size() > 14 )
			{
				days.erase( days.begin(), days.end() - 14 );
			}
			std::int64_t largest = 0;
			for ( const auto& [day, count] : days )
			{
				largest = std::max( largest, count );
			}
			QString rows;
			for ( const auto& [day, count] : days )
			{
				rows += bar( day, count, largest, QStringLiteral( "#9b7fe0" ) );
			}
			section( QStringLiteral( "The last days" ), rows, QStringLiteral( "No day has a lookup on it yet." ) );
		}

		html += QStringLiteral( "<br><p style=\"color: %1\">Kept on this computer only, in the state directory beside the logs (About → Files).</p>" ).arg( muted );
		const int scroll = detail_->verticalScrollBar() != nullptr ? detail_->verticalScrollBar()->value() : 0;
		detail_->setHtml( html );
		detail_->verticalScrollBar()->setValue( scroll );
	}

} // namespace lexiglance::gui
