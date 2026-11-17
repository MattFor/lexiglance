#include "SearchPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <lexiglance/language/Language.h>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QScrollBar>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace lexiglance::gui
{

	namespace
	{

		// Characters looked up from the start of the text; anki.add looks the text up the same way, so its entries match.
		constexpr int search_length = 32;

		QString categoryColor( std::string_view category )
		{
			if ( category == "name" )
			{
				return QStringLiteral( "#b6327a" );
			}
			if ( category == "expression" )
			{
				return QStringLiteral( "#c98a1f" );
			}
			if ( category == "popular" )
			{
				return QStringLiteral( "#1f6fc5" );
			}
			if ( category == "frequent" )
			{
				return QStringLiteral( "#2f93b0" );
			}
			if ( category == "archaism" )
			{
				return QStringLiteral( "#b8433f" );
			}
			if ( category == "partOfSpeech" )
			{
				return QStringLiteral( "#4d4f55" );
			}
			return QStringLiteral( "#5d5f66" );
		}

		QString pill( const QString& text, const QString& background )
		{
			return QStringLiteral( "<span style=\"background-color:%1; color:#ffffff; font-size:small; font-weight:bold;\">&nbsp;%2&nbsp;</span> " )
			        .arg( background, text.toHtmlEscaped() );
		}

		// Whether copied text is worth looking up: it has characters of a supported language's script.
		bool inKnownScript( const QString& text )
		{
			return std::ranges::any_of( text.toUcs4(), []( char32_t c ) {
				return std::ranges::any_of( lang::languages(), [c]( const lang::Language* language ) { return language->isScriptCharacter( c ); } );
			} );
		}

	} // namespace

	SearchPage::SearchPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		query_( new QLineEdit() ),
		results_( new QTextBrowser() ),
		timing_( new QLabel() ),
		watch_( new QCheckBox( QStringLiteral( "Watch clipboard" ) ) ),
		debounce_( new QTimer( this ) )
	{
		auto* layout = new QVBoxLayout( this );
		layout->setContentsMargins( 24, 20, 24, 20 );
		auto* row = new QHBoxLayout();
		query_->setPlaceholderText( QStringLiteral( "Type or paste a word or sentence (inflected forms work too)" ) );
		query_->setClearButtonEnabled( true );
		QFont font = query_->font();
		font.setPointSizeF( font.pointSizeF() * 1.4 );
		query_->setFont( font );
		row->addWidget( query_, 1 );
		timing_->setEnabled( false );
		row->addWidget( timing_ );
		watch_->setToolTip( QStringLiteral( "Looks up text as soon as it's copied, for example by a text hooker" ) );
		watch_->setChecked( applicationMemory().value( QStringLiteral( "search/watch_clipboard" ) ).toBool() );
		row->addWidget( watch_ );
		layout->addLayout( row );
		results_->setOpenLinks( false );
		results_->setPlaceholderText( QStringLiteral( "Results appear as you type. Words you look up on screen show up here too." ) );
		layout->addWidget( results_, 1 );
		query_->installEventFilter( this );

		debounce_->setSingleShot( true );
		debounce_->setInterval( 120 );
		connect( debounce_, &QTimer::timeout, this, [this] { search(); } );
		connect( query_, &QLineEdit::textChanged, debounce_, qOverload<>( &QTimer::start ) );

		connect( watch_, &QCheckBox::toggled, this, []( bool on ) { applicationMemory().setValue( QStringLiteral( "search/watch_clipboard" ), on ); } );
		connect( QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
			const QString text = QGuiApplication::clipboard()->text().trimmed();
			if ( watch_->isChecked() && text.size() <= 400 && inKnownScript( text ) && text != query_->text() && text != copied_ )
			{
				query_->setText( text );
			}
		} );

		// Cross references in dictionaries and the recent words link to "?query=word".
		connect( results_, &QTextBrowser::anchorClicked, this, [this]( const QUrl& url ) {
			if ( url.scheme() == QStringLiteral( "lexiglance" ) )
			{
				act( url );
				return;
			}
			const QString word = QUrlQuery( url.query() ).queryItemValue( QStringLiteral( "query" ), QUrl::FullyDecoded );
			if ( !word.isEmpty() )
			{
				query_->setText( word );
			}
		} );
	}

	void SearchPage::activated()
	{
		query_->setFocus();
	}

	// Audio or Anki turned on or off, dictionaries changed: the entries on show follow.
	void SearchPage::refresh()
	{
		search();
	}

	void SearchPage::setQuery( const QString& text )
	{
		query_->setText( text.trimmed() );
		query_->setFocus();
	}

	void SearchPage::setRecent( QStringList words )
	{
		recent_ = std::move( words );
		if ( query_->text().trimmed().isEmpty() )
		{
			showRecent();
		}
	}

	bool SearchPage::eventFilter( QObject* watched, QEvent* event )
	{
		if ( const auto* pressed = dynamic_cast<QKeyEvent*>( event ); watched == query_ && pressed != nullptr && event->type() == QEvent::KeyPress )
		{
			// The results scroll from the search box, which keeps the focus for the next word.
			const int  key    = pressed->key();
			const auto action = key == Qt::Key_Down     ? QAbstractSlider::SliderSingleStepAdd
			                  : key == Qt::Key_Up       ? QAbstractSlider::SliderSingleStepSub
			                  : key == Qt::Key_PageDown ? QAbstractSlider::SliderPageStepAdd
			                  : key == Qt::Key_PageUp   ? QAbstractSlider::SliderPageStepSub
			                                            : QAbstractSlider::SliderNoAction;
			if ( action != QAbstractSlider::SliderNoAction )
			{
				results_->verticalScrollBar()->triggerAction( action );
				return true;
			}
			if ( key == Qt::Key_Escape && !query_->text().isEmpty() )
			{
				query_->clear();
				return true;
			}
		}
		return Page::eventFilter( watched, event );
	}

	void SearchPage::search()
	{
		const QString text = query_->text().trimmed();
		if ( text.isEmpty() )
		{
			terms_.clear();
			shown_query_.clear();
			timing_->clear();
			showRecent();
			return;
		}
		json::Writer params;
		params.beginObject().field( "text", ss( text ) ).field( "markup", "html" ).field( "max_length", search_length ).endObject();
		client().call( "lookup", params.take(), [this, text]( const json::Value* result, const QString& error ) {
			// Typed on (or cleared) meanwhile: the next answer is the one to show.
			if ( text != query_->text().trimmed() )
			{
				return;
			}
			if ( result == nullptr )
			{
				results_->setHtml( "<p>" + error.toHtmlEscaped() + "</p>" );
				return;
			}
			show( *result, text );
		} );
	}

	void SearchPage::showRecent()
	{
		if ( recent_.isEmpty() )
		{
			results_->clear();
			return;
		}
		QStringList links;
		for ( const QString& word : recent_ )
		{
			links << QStringLiteral( "<a href=\"?query=%1\" style=\"text-decoration:none;\">%2</a>" ).arg( QString::fromLatin1( QUrl::toPercentEncoding( word ) ), word.toHtmlEscaped() );
		}
		results_->setHtml( QStringLiteral( "<p style=\"color:#8a8d94;\">Recently looked up</p><p style=\"font-size:18pt;\">%1</p>" ).arg( links.join( QStringLiteral( "&emsp; " ) ) ) );
	}

	void SearchPage::act( const QUrl& url )
	{
		const auto entry = QUrlQuery( url.query() ).queryItemValue( QStringLiteral( "entry" ) ).toULongLong();
		if ( entry >= terms_.size() )
		{
			return;
		}
		const Term&   term   = terms_[entry];
		const QString action = url.path();
		json::Writer  params;
		if ( action == QStringLiteral( "copy" ) )
		{
			copied_ = term.expression;
			QGuiApplication::clipboard()->setText( term.expression );
			timing_->setText( QStringLiteral( "Copied %1" ).arg( term.expression ) );
		}
		else if ( action == QStringLiteral( "audio" ) )
		{
			params.beginObject().field( "expression", ss( term.expression ) ).field( "reading", ss( term.reading ) ).endObject();
			timing_->setText( QStringLiteral( "Playing %1…" ).arg( term.expression ) );
			client().call(
					"audio.play",
					params.take(),
					[this, word = term.expression]( const json::Value* result, const QString& error ) { timing_->setText( result != nullptr ? QStringLiteral( "Played %1" ).arg( word ) : error ); },
					25000
			);
		}
		else if ( action == QStringLiteral( "anki" ) )
		{
			params.beginObject().field( "text", ss( shown_query_ ) ).field( "entry", entry ).field( "max_length", search_length ).endObject();
			timing_->setText( QStringLiteral( "Adding %1 to Anki…" ).arg( term.expression ) );
			client().call(
					"anki.add",
					params.take(),
					[this, word = term.expression]( const json::Value* result, const QString& error ) { timing_->setText( result != nullptr ? QStringLiteral( "Added %1 to Anki" ).arg( word ) : error ); },
					35000
			);
		}
	}

	void SearchPage::show( const json::Value& result, const QString& text )
	{
		// The same text again (the settings changed): it stays scrolled where it was.
		const int scrolled = text == shown_query_ ? results_->verticalScrollBar()->value() : 0;
		shown_query_       = text;
		terms_.clear();
		timing_->setText( QStringLiteral( "%1 µs" ).arg( result["elapsed_us"].asInt() ) );
		const auto& config = settings().config();
		QString     html;
		for ( const json::Value& term : result["terms"].items() )
		{
			const QString expression = qs( term["expression"].asString() );
			const QString reading    = qs( term["reading"].asString() );
			const auto    entry      = terms_.size();
			terms_.push_back( { .expression = expression, .reading = reading } );

			html += QStringLiteral( "<div style=\"margin-bottom:18px;\">" );
			if ( reading != expression )
			{
				html += QStringLiteral( "<div style=\"color:#8a8d94;\">%1</div>" ).arg( reading.toHtmlEscaped() );
			}
			html += QStringLiteral( "<div style=\"font-size:24pt;\">%1</div>" ).arg( expression.toHtmlEscaped() );
			if ( const auto inflections = qs( term["inflections"].asString() ); !inflections.isEmpty() )
			{
				html += QStringLiteral( "<div style=\"color:#8a8d94; font-style:italic;\">« %1</div>" ).arg( inflections.toHtmlEscaped() );
			}
			QStringList actions;
			const auto  link = [entry]( const QString& action, const QString& label ) { return QStringLiteral( "<a href=\"lexiglance:%1?entry=%2\">%3</a>" ).arg( action ).arg( entry ).arg( label ); };
			if ( config.audio.enabled )
			{
				actions << link( QStringLiteral( "audio" ), QStringLiteral( "Play" ) );
			}
			actions << link( QStringLiteral( "copy" ), QStringLiteral( "Copy" ) );
			if ( config.anki.enabled )
			{
				actions << link( QStringLiteral( "anki" ), QStringLiteral( "Add to Anki" ) );
			}
			html += QStringLiteral( "<div style=\"margin:2px 0 4px 0;\">%1</div>" ).arg( actions.join( QStringLiteral( "&nbsp;&nbsp;·&nbsp;&nbsp;" ) ) );

			QString badges;
			for ( const json::Value& f : term["frequencies"].items() )
			{
				const QString value = f["display"].asString().empty() ? QString::number( f["value"].asInt() ) : qs( f["display"].asString() );
				badges += pill( shortTitle( qs( f["dictionary"].asString() ) ) + QStringLiteral( " " ) + value, QStringLiteral( "#3d8f47" ) );
			}
			if ( !badges.isEmpty() )
			{
				html += "<div style=\"margin:4px 0;\">" + badges + "</div>";
			}

			QString last_dictionary;
			for ( const json::Value& definition : term["definitions"].items() )
			{
				const QString dictionary = qs( definition["dictionary"].asString() );
				QString       tags;
				for ( const json::Value& tag : definition["tags"].items() )
				{
					const auto name = tag["name"].asString();
					if ( !name.empty() && !std::ranges::all_of( name, []( char c ) { return c >= '0' && c <= '9'; } ) )
					{
						tags += pill( qs( name ), categoryColor( tag["category"].asString() ) );
					}
				}
				if ( dictionary != last_dictionary )
				{
					tags += pill( shortTitle( dictionary ), QStringLiteral( "#8e55b3" ) );
					last_dictionary = dictionary;
				}
				html += "<div style=\"margin-top:6px;\">" + tags + "</div><div style=\"margin-left:14px;\">";
				for ( const json::Value& gloss : definition["glossary"].items() )
				{
					html += "<div>" + qs( gloss.asString() ) + "</div>";
				}
				html += QStringLiteral( "</div>" );
			}
			html += QStringLiteral( "</div><hr>" );
		}

		for ( const json::Value& kanji : result["kanji"].items() )
		{
			QStringList meanings;
			for ( const json::Value& meaning : kanji["meanings"].items() )
			{
				meanings << qs( meaning.asString() );
			}
			html += QStringLiteral( "<div><span style=\"font-size:40pt;\">%1</span><br><b>%2</b><br>On: %3<br>Kun: %4</div>" )
			                .arg( qs( kanji["character"].asString() ).toHtmlEscaped(),
			                      meanings.join( QStringLiteral( ", " ) ).toHtmlEscaped(),
			                      qs( kanji["onyomi"].asString() ).toHtmlEscaped(),
			                      qs( kanji["kunyomi"].asString() ).toHtmlEscaped() );
		}

		results_->setHtml( html.isEmpty() ? QStringLiteral( "<p>No results.</p>" ) : html );
		results_->verticalScrollBar()->setValue( scrolled );
	}

} // namespace lexiglance::gui
