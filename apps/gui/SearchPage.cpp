#include "SearchPage.h"

#include "DaemonClient.h"

#include <lexiglance/language/Language.h>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace lexiglance::gui
{

	namespace
	{

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
		query_->setPlaceholderText( QStringLiteral( "Type or paste a word or a sentence (inflected forms work too)" ) );
		query_->setClearButtonEnabled( true );
		QFont font = query_->font();
		font.setPointSizeF( font.pointSizeF() * 1.4 );
		query_->setFont( font );
		row->addWidget( query_, 1 );
		timing_->setEnabled( false );
		row->addWidget( timing_ );
		watch_->setToolTip( QStringLiteral( "Look up text as soon as it is copied, e.g. from a text hooker" ) );
		row->addWidget( watch_ );
		layout->addLayout( row );
		results_->setOpenLinks( false );
		results_->setPlaceholderText( QStringLiteral( "Results appear as you type." ) );
		layout->addWidget( results_, 1 );

		debounce_->setSingleShot( true );
		debounce_->setInterval( 120 );
		connect( debounce_, &QTimer::timeout, this, [this] { search(); } );
		connect( query_, &QLineEdit::textChanged, debounce_, qOverload<>( &QTimer::start ) );

		connect( QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
			const QString text = QGuiApplication::clipboard()->text().trimmed();
			if ( watch_->isChecked() && text.size() <= 400 && inKnownScript( text ) && text != query_->text() )
			{
				query_->setText( text );
			}
		} );

		// Cross references in dictionaries link to "?query=word".
		connect( results_, &QTextBrowser::anchorClicked, this, [this]( const QUrl& url ) {
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

	void SearchPage::setQuery( const QString& text )
	{
		query_->setText( text.trimmed() );
		query_->setFocus();
	}

	void SearchPage::search()
	{
		if ( query_->text().trimmed().isEmpty() )
		{
			results_->clear();
			timing_->clear();
			return;
		}
		json::Writer params;
		params.beginObject().field( "text", ss( query_->text().trimmed() ) ).field( "markup", "html" ).field( "max_length", 32 ).endObject();
		client().call( "lookup", params.take(), [this]( const json::Value* result, const QString& error ) {
			if ( result == nullptr )
			{
				results_->setHtml( "<p>" + error.toHtmlEscaped() + "</p>" );
				return;
			}
			show( *result );
		} );
	}

	void SearchPage::show( const json::Value& result )
	{
		timing_->setText( QStringLiteral( "%1 µs" ).arg( result["elapsed_us"].asInt() ) );
		QString html;
		for ( const json::Value& term : result["terms"].items() )
		{
			const QString expression = qs( term["expression"].asString() );
			const QString reading    = qs( term["reading"].asString() );

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
	}

} // namespace lexiglance::gui
