#include "StatisticsPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/language/Language.h>

#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		QFrame* tile( QLabel* value, QLabel* label )
		{
			auto* frame = new QFrame();
			frame->setObjectName( QStringLiteral( "tile" ) );
			auto* layout = new QVBoxLayout( frame );
			layout->setContentsMargins( 12, 8, 12, 8 );
			layout->setSpacing( 2 );
			value->setObjectName( QStringLiteral( "tileValue" ) );
			value->setTextInteractionFlags( Qt::TextSelectableByMouse );
			label->setObjectName( QStringLiteral( "tileCaption" ) );
			label->setWordWrap( true );
			layout->addWidget( value );
			layout->addWidget( label );
			return frame;
		}

		// One of a row of choices, shown as a rounded button that is filled while chosen.
		QPushButton* pill( const QString& text, QButtonGroup* group, int id )
		{
			auto* button = new QPushButton( text );
			button->setCheckable( true );
			button->setProperty( "pill", true );
			button->setCursor( Qt::PointingHandCursor );
			group->addButton( button, id );
			return button;
		}

		// A titled card around one of the lists.
		QGroupBox* card( const QString& title, QWidget* body )
		{
			auto* box    = new QGroupBox( title );
			auto* layout = new QVBoxLayout( box );
			layout->addWidget( body );
			layout->addStretch( 1 );
			return box;
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
		range_( new QButtonGroup( this ) ),
		figure_( new QComboBox() ),
		style_( new QButtonGroup( this ) ),
		chart_( new ActivityChart() ),
		words_( new BarList() ),
		languages_( new BarList() ),
		translated_( new BarList() ),
		sources_( new BarList() ),
		counting_( new QCheckBox( QStringLiteral( "Count what I look up" ) ) ),
		counting_translations_( new QCheckBox( QStringLiteral( "Count translations" ) ) ),
		note_( new QLabel() ),
		poll_( new QTimer( this ) )
	{
		auto* scroll  = new QScrollArea();
		auto* content = new QWidget();
		auto* layout  = new QVBoxLayout( content );
		layout->setContentsMargins( 28, 24, 28, 20 );
		layout->setSpacing( 14 );

		// The title, and the stretch of time everything below is about.
		auto* top = new QHBoxLayout();
		top->addWidget( heading( QStringLiteral( "Statistics" ) ) );
		top->addStretch( 1 );
		for ( const auto& [days, name] : { std::pair{ 7, "7 days" }, std::pair{ 30, "30 days" }, std::pair{ 90, "90 days" }, std::pair{ 365, "A year" }, std::pair{ 0, "All time" } } )
		{
			top->addWidget( pill( QString::fromLatin1( name ), range_, days ) );
		}
		layout->addLayout( top );
		note_->setWordWrap( true );
		note_->setEnabled( false );
		layout->addWidget( note_ );

		// Three rows of four figures, the same tiles the overview uses.
		auto* grid = new QGridLayout();
		grid->setSpacing( 10 );
		for ( std::size_t i = 0; i < values_.size(); ++i )
		{
			values_[i]   = new QLabel( QStringLiteral( "–" ) );
			captions_[i] = new QLabel();
			grid->addWidget( tile( values_[i], captions_[i] ), static_cast<int>( i / 4 ), static_cast<int>( i % 4 ) );
		}
		layout->addLayout( grid );

		// One figure day by day; the pointer over a day shows all of that day's.
		auto* activity        = new QGroupBox( QStringLiteral( "Activity" ) );
		auto* activity_layout = new QVBoxLayout( activity );
		auto* controls        = new QHBoxLayout();
		for ( std::size_t i = 0; i < figure_count; ++i )
		{
			figure_->addItem( figureName( static_cast<Figure>( i ) ), static_cast<int>( i ) );
		}
		controls->addWidget( figure_ );
		controls->addStretch( 1 );
		auto* hint = new QLabel( QStringLiteral( "Point at a day to see all of it" ) );
		hint->setEnabled( false );
		controls->addWidget( hint );
		controls->addSpacing( 12 );
		controls->addWidget( pill( QStringLiteral( "Bars" ), style_, static_cast<int>( ActivityChart::Style::Bars ) ) );
		controls->addWidget( pill( QStringLiteral( "Line" ), style_, static_cast<int>( ActivityChart::Style::Line ) ) );
		activity_layout->addLayout( controls );
		activity_layout->addWidget( chart_ );
		layout->addWidget( activity );

		// What came back most, side by side.
		auto* lists = new QGridLayout();
		lists->setSpacing( 12 );
		lists->addWidget( card( QStringLiteral( "Words you look up most" ), words_ ), 0, 0, 3, 1 );
		lists->addWidget( card( QStringLiteral( "Languages" ), languages_ ), 0, 1 );
		lists->addWidget( card( QStringLiteral( "Translated from" ), translated_ ), 1, 1 );
		lists->addWidget( card( QStringLiteral( "How the text was read" ), sources_ ), 2, 1 );
		lists->setColumnStretch( 0, 1 );
		lists->setColumnStretch( 1, 1 );
		layout->addLayout( lists );
		layout->addStretch( 1 );

		auto*         row  = new QHBoxLayout();
		const QString kept = QStringLiteral( "The tally is kept in this computer's state directory and is never sent anywhere. Turning this off stops counting them; what was counted stays until it is forgotten." );
		counting_->setToolTip( QStringLiteral( "Lookups, the words found, the popups shown, sounds played and Anki cards made. " ) + kept );
		counting_translations_->setToolTip( QStringLiteral( "The sentences translated, their languages and how long the translations took. " ) + kept );
		row->addWidget( counting_ );
		row->addSpacing( 16 );
		row->addWidget( counting_translations_ );
		row->addStretch( 1 );
		auto* forget = new QPushButton( QStringLiteral( "Forget everything..." ) );
		row->addWidget( forget );
		layout->addLayout( row );

		scroll->setWidget( content );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		auto* outer = new QVBoxLayout( this );
		outer->setContentsMargins( 0, 0, 0, 0 );
		outer->addWidget( scroll );

		// The choices of the last time.
		const auto memory = applicationMemory();
		if ( auto* button = range_->button( memory.value( QStringLiteral( "statistics/range" ), 30 ).toInt() ) )
		{
			button->setChecked( true );
		}
		figure_->setCurrentIndex( std::clamp( memory.value( QStringLiteral( "statistics/figure" ), 0 ).toInt(), 0, static_cast<int>( figure_count ) - 1 ) );
		if ( auto* button = style_->button( memory.value( QStringLiteral( "statistics/style" ), 0 ).toInt() ) )
		{
			button->setChecked( true );
		}

		connect( range_, &QButtonGroup::idClicked, this, [this] {
			remember();
			show();
		} );
		connect( style_, &QButtonGroup::idClicked, this, [this] {
			remember();
			show();
		} );
		connect( figure_, &QComboBox::currentIndexChanged, this, [this] {
			remember();
			show();
		} );
		connect( counting_, &QCheckBox::toggled, this, [this]( bool on ) {
			settings().config().statistics = on;
			settings().commit();
			update();
		} );
		connect( counting_translations_, &QCheckBox::toggled, this, [this]( bool on ) {
			settings().config().statistics_translations = on;
			settings().commit();
			update();
		} );
		connect( forget, &QPushButton::clicked, this, [this] {
			if ( !confirm( this, QStringLiteral( "Forget the statistics" ), QStringLiteral( "Everything counted so far is forgotten: the words, the languages, the translations, the days. The dictionaries and settings are untouched." ), QStringLiteral( "Forget" ) ) )
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
		show();
	}

	void StatisticsPage::activated()
	{
		update();
	}

	void StatisticsPage::refresh()
	{
		const QSignalBlocker blocker( counting_ );
		const QSignalBlocker translations_blocker( counting_translations_ );
		counting_->setChecked( settings().config().statistics );
		counting_translations_->setChecked( settings().config().statistics_translations );
	}

	void StatisticsPage::remember() const
	{
		auto memory = applicationMemory();
		memory.setValue( QStringLiteral( "statistics/range" ), range_->checkedId() );
		memory.setValue( QStringLiteral( "statistics/figure" ), figure_->currentIndex() );
		memory.setValue( QStringLiteral( "statistics/style" ), style_->checkedId() );
	}

	void StatisticsPage::update()
	{
		client().call( "stats", "{}", [this]( const json::Value* stats, const QString& error ) {
			if ( stats == nullptr )
			{
				// A daemon from before this page knows no such request; it counts nothing until it is restarted.
				if ( error.contains( QStringLiteral( "unknown method" ) ) )
				{
					note_->setText( QStringLiteral( "The running daemon is older than this window and counts nothing yet. Restart Lexiglance (Overview → Restart) to start the tally." ) );
				}
				else
				{
					note_->setText( error.isEmpty() ? QStringLiteral( "Lexiglance is not running, so there is nothing to count with." ) : error );
				}
				return;
			}
			read( *stats );
			show();
		} );
	}

	void StatisticsPage::read( const json::Value& stats )
	{
		Tally tally;
		tally.enabled              = stats["enabled"].asBool();
		tally.translations_enabled = stats["translations_enabled"].isBool() ? stats["translations_enabled"].asBool() : tally.enabled;
		tally.first_day            = QDate::fromString( qs( stats["first_day"].asString() ), Qt::ISODate );
		tally.sessions             = stats["sessions"].asInt();
		tally.distinct_words       = stats["distinct_words"].asInt();
		tally.lookup_us            = stats["average_lookup_us"].asDouble();
		tally.translation_ms       = stats["average_translation_ms"].asDouble();
		for ( std::size_t i = 0; i < figure_count; ++i )
		{
			tally.totals[i] = stats[figureKey( static_cast<Figure>( i ) )].asInt();
		}
		for ( const json::Value& item : stats["days"].items() )
		{
			const QDate day = QDate::fromString( qs( item["day"].asString() ), Qt::ISODate );
			if ( !day.isValid() )
			{
				continue;
			}
			auto& values = tally.days[day];
			for ( std::size_t i = 0; i < figure_count; ++i )
			{
				values[i] = item[figureKey( static_cast<Figure>( i ) )].asInt();
			}
			// A daemon from before 1.3.0 counts only the lookups of a day.
			if ( item.find( "lookups" ) == nullptr )
			{
				values[static_cast<std::size_t>( Figure::Lookups )] = item["count"].asInt();
			}
		}
		const auto rows = [&]( const char* list, const char* key, const std::function<QString( const QString& )>& name ) {
			std::vector<BarList::Row> out;
			for ( const json::Value& item : stats[list].items() )
			{
				out.push_back( { .label = name( qs( item[key].asString() ) ), .count = item["count"].asInt() } );
			}
			return out;
		};
		tally.words      = rows( "words", "word", []( const QString& word ) { return word; } );
		tally.languages  = rows( "languages", "language", languageName );
		tally.translated = rows( "translated_languages", "language", languageName );
		tally.sources    = rows( "sources", "source", sourceName );
		tally_           = std::move( tally );
		loaded_          = true;
	}

	std::vector<Period> StatisticsPage::periods() const
	{
		const QDate today = QDate::currentDate();
		const int   range = range_->checkedId();
		QDate       first = range > 0 ? today.addDays( 1 - range ) : tally_.first_day;
		if ( range <= 0 && !tally_.days.empty() && ( !first.isValid() || tally_.days.begin()->first < first ) )
		{
			first = tally_.days.begin()->first;
		}
		if ( !first.isValid() || first > today )
		{
			first = today;
		}
		// Long stretches are shown by the week, ending today, so every bar stays wide enough to point at.
		const auto          length = first.daysTo( today ) + 1;
		const int           width  = length > 120 ? 7 : 1;
		std::vector<Period> out;
		for ( QDate end = today; end >= first; end = end.addDays( -width ) )
		{
			Period period{ .first = std::max( first, end.addDays( 1 - width ) ), .last = end };
			for ( auto it = tally_.days.lower_bound( period.first ); it != tally_.days.end() && it->first <= period.last; ++it )
			{
				for ( std::size_t i = 0; i < figure_count; ++i )
				{
					period.values[i] += it->second[i];
				}
			}
			out.push_back( period );
		}
		std::ranges::reverse( out );
		return out;
	}

	void StatisticsPage::show()
	{
		const int  range    = range_->checkedId();
		const bool all_time = range <= 0;
		const auto chosen   = periods();

		// The figures of the range: summed from its days, or the totals kept since counting began.
		std::array<std::int64_t, figure_count> sums{};
		std::int64_t                           active = 0;
		for ( const Period& period : chosen )
		{
			for ( std::size_t i = 0; i < figure_count; ++i )
			{
				sums[i] += period.values[i];
			}
		}
		for ( const auto& [day, values] : tally_.days )
		{
			const bool inside = all_time || day > QDate::currentDate().addDays( -range );
			active += inside && std::ranges::any_of( values, []( std::int64_t value ) { return value > 0; } ) ? 1 : 0;
		}
		if ( all_time )
		{
			sums = tally_.totals;
		}
		const QString ever = all_time ? QString() : QStringLiteral( " (all time)" );
		const auto    set  = [&]( std::size_t tile, const QString& value, const QString& caption ) {
			values_[tile]->setText( loaded_ ? value : QStringLiteral( "–" ) );
			captions_[tile]->setText( caption );
		};
		const auto sum = [&]( Figure figure ) { return number( sums[static_cast<std::size_t>( figure )] ); };
		set( 0, sum( Figure::Lookups ), QStringLiteral( "Words looked up" ) );
		set( 1, sum( Figure::Found ), QStringLiteral( "With an entry" ) );
		set( 2, sum( Figure::Characters ), QStringLiteral( "Characters read" ) );
		set( 3, number( active ), QStringLiteral( "Days used" ) );
		set( 4, sum( Figure::Popups ), QStringLiteral( "Popups shown" ) );
		set( 5, sum( Figure::Audio ), QStringLiteral( "Pronunciations played" ) );
		set( 6, sum( Figure::Anki ), QStringLiteral( "Anki notes added" ) );
		set( 7, number( tally_.distinct_words ), QStringLiteral( "Different words" ) + ever );
		set( 8, sum( Figure::Translations ), QStringLiteral( "Translations" ) );
		set( 9, sum( Figure::TranslatedCharacters ), QStringLiteral( "Characters translated" ) );
		set( 10, tally_.translation_ms > 0 ? QStringLiteral( "%1 ms" ).arg( tally_.translation_ms, 0, 'f', 0 ) : QStringLiteral( "–" ), QStringLiteral( "Per translation" ) + ever );
		set( 11, tally_.lookup_us > 0 ? QStringLiteral( "%1 µs" ).arg( tally_.lookup_us, 0, 'f', 0 ) : QStringLiteral( "–" ), QStringLiteral( "Per lookup" ) + ever );

		QString summary;
		if ( !loaded_ )
		{
			summary = QStringLiteral( "Asking Lexiglance for its tally..." );
		}
		else if ( tally_.totals[static_cast<std::size_t>( Figure::Lookups )] == 0 && tally_.totals[static_cast<std::size_t>( Figure::Translations )] == 0 )
		{
			summary = QStringLiteral( "Nothing has been looked up yet. Hold the trigger over a word and this page fills itself." );
		}
		else
		{
			summary = QStringLiteral( "Counting since %1, over %2." ).arg( tally_.first_day.isValid() ? QLocale().toString( tally_.first_day, QLocale::LongFormat ) : QStringLiteral( "the first lookup" ), tally_.sessions == 1 ? QStringLiteral( "one session" ) : QStringLiteral( "%1 sessions" ).arg( number( tally_.sessions ) ) );
		}
		if ( loaded_ && !tally_.enabled && !tally_.translations_enabled )
		{
			summary += QStringLiteral( " Counting is off, so these figures stand still." );
		}
		else if ( loaded_ && !tally_.enabled )
		{
			summary += QStringLiteral( " Lookups are not counted, so their figures stand still." );
		}
		else if ( loaded_ && !tally_.translations_enabled )
		{
			summary += QStringLiteral( " Translations are not counted, so their figures stand still." );
		}
		note_->setText( summary );

		chart_->setPeriods( chosen, static_cast<Figure>( std::max( 0, figure_->currentIndex() ) ), static_cast<ActivityChart::Style>( std::max( 0, style_->checkedId() ) ) );
		words_->setRows( std::vector<BarList::Row>( tally_.words.begin(), tally_.words.begin() + static_cast<std::ptrdiff_t>( std::min<std::size_t>( tally_.words.size(), 15 ) ) ), QColor( 0x5b, 0x8d, 0xef ), QStringLiteral( "No word has been found yet." ) );
		languages_->setRows( tally_.languages, QColor( 0x3f, 0xa4, 0x5b ), QStringLiteral( "Nothing has been found in any language yet." ) );
		translated_->setRows( tally_.translated, QColor( 0xd0, 0x6a, 0x9c ), QStringLiteral( "Nothing has been translated yet." ) );
		sources_->setRows( tally_.sources, QColor( 0xd1, 0x9a, 0x1f ), QStringLiteral( "Nothing has been read from the screen yet." ) );
	}

} // namespace lexiglance::gui
