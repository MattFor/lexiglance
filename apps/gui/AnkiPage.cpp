#include "AnkiPage.h"

#include "DaemonClient.h"
#include "Settings.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace lexiglance::gui
{

	namespace
	{

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			label->setTextInteractionFlags( Qt::TextSelectableByMouse );
			return label;
		}

		// A sensible template for a field, judged by its name.
		QString guessTemplate( const QString& field, bool first )
		{
			const QString name = field.toLower();
			const auto    has  = [&]( std::initializer_list<const char*> words ) {
                return std::ranges::any_of( words, [&]( const char* word ) { return name.contains( QString::fromUtf8( word ) ); } );
			};
			if ( has( { "audio", "sound", "音声" } ) )
			{
				return QStringLiteral( "{audio}" );
			}
			if ( has( { "sentence", "例文", "文" } ) )
			{
				return has( { "cloze" } ) ? QStringLiteral( "{cloze-prefix}{{c1::{cloze-body}}}{cloze-suffix}" ) : QStringLiteral( "{cloze-prefix}<b>{cloze-body}</b>{cloze-suffix}" );
			}
			if ( has( { "furigana" } ) )
			{
				return QStringLiteral( "{furigana-plain}" );
			}
			if ( has( { "reading", "kana", "読み" } ) )
			{
				return QStringLiteral( "{reading}" );
			}
			if ( has( { "meaning", "definition", "glossary", "english", "back", "意味" } ) )
			{
				return QStringLiteral( "{glossary}" );
			}
			if ( has( { "pitch" } ) )
			{
				return QStringLiteral( "{pitch-accents}" );
			}
			if ( has( { "freq" } ) )
			{
				return QStringLiteral( "{frequencies}" );
			}
			if ( first || has( { "expression", "word", "vocab", "front", "単語" } ) )
			{
				return QStringLiteral( "{expression}" );
			}
			return {};
		}

	} // namespace

	AnkiPage::AnkiPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		network_( new QNetworkAccessManager( this ) ),
		audio_enabled_( new QCheckBox( QStringLiteral( "Show a play button on every entry" ) ) ),
		autoplay_( new QCheckBox( QStringLiteral( "Play the first entry automatically" ) ) ),
		sources_( new QPlainTextEdit() ),
		test_audio_( new QPushButton( QIcon::fromTheme( QStringLiteral( "audio-volume-high" ) ), QStringLiteral( "Test" ) ) ),
		audio_status_( new QLabel() ),
		anki_enabled_( new QCheckBox( QStringLiteral( "Show an \"add to Anki\" button on every entry" ) ) ),
		url_( new QLineEdit() ),
		key_( new QLineEdit() ),
		connect_( new QPushButton( QIcon::fromTheme( QStringLiteral( "network-connect" ) ), QStringLiteral( "Connect" ) ) ),
		anki_status_( new QLabel() ),
		deck_( new QComboBox() ),
		model_( new QComboBox() ),
		fields_( new QTableWidget( 0, 2 ) ),
		tags_( new QLineEdit() ),
		duplicates_( new QCheckBox( QStringLiteral( "Allow duplicate notes" ) ) )
	{
		auto* scroll  = new QScrollArea();
		auto* content = new QWidget();
		auto* layout  = new QVBoxLayout( content );
		layout->setContentsMargins( 24, 20, 24, 20 );
		layout->setSpacing( 14 );

		auto* audio_box = new QGroupBox( QStringLiteral( "Audio" ) );
		auto* audio     = new QVBoxLayout( audio_box );
		audio->addWidget( audio_enabled_ );
		audio->addWidget( autoplay_ );
		audio->addWidget( note( QStringLiteral( "Sources, tried in order: <b>jpod101</b> (JapanesePod101, Japanese words), <b>commons</b> (Wikimedia Commons, many languages) "
		                                        "or a URL with {term}, {reading} and {language}, e.g. a local audio server. Words no source has are looked for on Wikimedia Commons. "
		                                        "Clips are played with ffplay, mpv or mpg123." ) ) );
		sources_->setMaximumHeight( 70 );
		sources_->setPlaceholderText( QStringLiteral( "jpod101\ncommons" ) );
		audio->addWidget( sources_ );
		auto* audio_row = new QHBoxLayout();
		audio_row->addWidget( test_audio_ );
		audio_status_->setEnabled( false );
		audio_row->addWidget( audio_status_, 1 );
		audio->addLayout( audio_row );
		layout->addWidget( audio_box );

		auto* anki_box = new QGroupBox( QStringLiteral( "Anki" ) );
		auto* anki     = new QVBoxLayout( anki_box );
		anki->addWidget( note( QStringLiteral( "Install the <a href=\"https://ankiweb.net/shared/info/2055492159\">AnkiConnect</a> add-on and keep Anki "
		                                       "running. Notes get the word, reading, definitions, the sentence it was found in and its audio." ) ) );
		qobject_cast<QLabel*>( anki->itemAt( 0 )->widget() )->setOpenExternalLinks( true );
		anki->addWidget( anki_enabled_ );
		auto* form    = new QFormLayout();
		auto* url_row = new QHBoxLayout();
		url_->setPlaceholderText( QStringLiteral( "http://127.0.0.1:8765" ) );
		url_row->addWidget( url_, 1 );
		url_row->addWidget( connect_ );
		form->addRow( QStringLiteral( "AnkiConnect" ), url_row );
		key_->setEchoMode( QLineEdit::Password );
		key_->setPlaceholderText( QStringLiteral( "only if AnkiConnect requires one" ) );
		form->addRow( QStringLiteral( "API key" ), key_ );
		form->addRow( QStringLiteral( "Deck" ), deck_ );
		form->addRow( QStringLiteral( "Note type" ), model_ );
		tags_->setPlaceholderText( QStringLiteral( "space separated" ) );
		form->addRow( QStringLiteral( "Tags" ), tags_ );
		form->addRow( duplicates_ );
		anki->addLayout( form );
		anki_status_->setWordWrap( true );
		anki_status_->setEnabled( false );
		anki->addWidget( anki_status_ );

		fields_->setHorizontalHeaderLabels( { QStringLiteral( "Field" ), QStringLiteral( "Content" ) } );
		fields_->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
		fields_->horizontalHeader()->setStretchLastSection( true );
		fields_->verticalHeader()->hide();
		fields_->setMinimumHeight( 180 );
		anki->addWidget( fields_ );
		QStringList markers;
		for ( const auto marker : config::anki_markers )
		{
			markers << QStringLiteral( "{%1}" ).arg( qs( marker ) );
		}
		anki->addWidget( note( QStringLiteral( "Markers: " ) + markers.join( QStringLiteral( " " ) ) ) );
		layout->addWidget( anki_box );
		layout->addStretch( 1 );

		scroll->setWidget( content );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		auto* outer = new QVBoxLayout( this );
		outer->setContentsMargins( 0, 0, 0, 0 );
		outer->addWidget( scroll );

		connect( audio_enabled_, &QCheckBox::toggled, this, [this] { storeAudio(); } );
		connect( autoplay_, &QCheckBox::toggled, this, [this] { storeAudio(); } );
		connect( sources_, &QPlainTextEdit::textChanged, this, [this] { storeAudio(); } );
		connect( test_audio_, &QPushButton::clicked, this, [this] {
			audio_status_->setText( QStringLiteral( "Playing 食べる…" ) );
			client().call( "audio.play", R"({"expression":"食べる","reading":"たべる"})", [this]( const json::Value* result, const QString& error ) {
				audio_status_->setText( result != nullptr ? QStringLiteral( "Played 食べる (たべる)." ) : error );
			} );
		} );

		connect( anki_enabled_, &QCheckBox::toggled, this, [this]( bool on ) {
			storeAnki();
			if ( on && deck_->count() <= 1 )
			{
				connectAnki();
			}
		} );
		connect( url_, &QLineEdit::editingFinished, this, [this] { storeAnki(); } );
		connect( key_, &QLineEdit::editingFinished, this, [this] { storeAnki(); } );
		connect( tags_, &QLineEdit::editingFinished, this, [this] { storeAnki(); } );
		connect( duplicates_, &QCheckBox::toggled, this, [this] { storeAnki(); } );
		connect( connect_, &QPushButton::clicked, this, [this] { connectAnki(); } );
		connect( deck_, &QComboBox::currentTextChanged, this, [this] { storeAnki(); } );
		connect( model_, &QComboBox::currentTextChanged, this, [this]( const QString& model ) {
			if ( !loading_ && !model.isEmpty() )
			{
				loadFields( model );
			}
		} );

		// While shown, AnkiConnect is asked every few seconds, so Anki starting or closing shows without leaving the page.
		auto* poll = new QTimer( this );
		poll->setInterval( 3000 );
		connect( poll, &QTimer::timeout, this, [this] {
			if ( !isVisible() || !anki_enabled_->isChecked() || anki_asking_ )
			{
				return;
			}
			anki_asking_ = true;
			request( QStringLiteral( "version" ), {}, [this]( const QJsonValue&, const QString& error ) {
				anki_asking_ = false;
				// Only a change is shown: Anki started (its decks and note types come in) or closed.
				if ( error.isEmpty() != anki_connected_ )
				{
					connectAnki();
				}
			} );
		} );
		poll->start();
	}

	void AnkiPage::refresh()
	{
		loading_           = true;
		const auto& config = settings().config();
		{
			const QSignalBlocker a( audio_enabled_ );
			const QSignalBlocker b( autoplay_ );
			const QSignalBlocker c( sources_ );
			const QSignalBlocker d( anki_enabled_ );
			const QSignalBlocker e( duplicates_ );
			audio_enabled_->setChecked( config.audio.enabled );
			autoplay_->setChecked( config.audio.autoplay );
			QStringList sources;
			for ( const auto& source : config.audio.sources )
			{
				sources << qs( source );
			}
			if ( sources_->toPlainText() != sources.join( '\n' ) )
			{
				sources_->setPlainText( sources.join( '\n' ) );
			}
			anki_enabled_->setChecked( config.anki.enabled );
			duplicates_->setChecked( config.anki.allow_duplicates );
		}
		url_->setText( qs( config.anki.url ) );
		key_->setText( qs( config.anki.key ) );
		QStringList tags;
		for ( const auto& tag : config.anki.tags )
		{
			tags << qs( tag );
		}
		tags_->setText( tags.join( ' ' ) );

		// Until AnkiConnect answers, the saved choices are the only ones.
		if ( deck_->findText( qs( config.anki.deck ) ) < 0 && !config.anki.deck.empty() )
		{
			deck_->addItem( qs( config.anki.deck ) );
		}
		deck_->setCurrentText( qs( config.anki.deck ) );
		if ( model_->findText( qs( config.anki.model ) ) < 0 && !config.anki.model.empty() )
		{
			model_->addItem( qs( config.anki.model ) );
		}
		model_->setCurrentText( qs( config.anki.model ) );

		QStringList names;
		for ( const auto& field : config.anki.fields )
		{
			names << qs( field.name );
		}
		showFields( names );
		loading_ = false;
	}

	void AnkiPage::activated()
	{
		if ( anki_enabled_->isChecked() && deck_->count() <= 1 )
		{
			connectAnki();
		}
	}

	void AnkiPage::storeAudio()
	{
		if ( loading_ )
		{
			return;
		}
		auto& audio    = settings().config().audio;
		audio.enabled  = audio_enabled_->isChecked();
		audio.autoplay = autoplay_->isChecked();
		audio.sources.clear();
		for ( const QString& line : sources_->toPlainText().split( '\n', Qt::SkipEmptyParts ) )
		{
			if ( !line.trimmed().isEmpty() )
			{
				audio.sources.push_back( ss( line.trimmed() ) );
			}
		}
		settings().commit();
	}

	void AnkiPage::storeAnki()
	{
		if ( loading_ )
		{
			return;
		}
		auto& anki            = settings().config().anki;
		anki.enabled          = anki_enabled_->isChecked();
		anki.url              = url_->text().trimmed().isEmpty() ? std::string( "http://127.0.0.1:8765" ) : ss( url_->text().trimmed() );
		anki.key              = ss( key_->text() );
		anki.deck             = ss( deck_->currentText() );
		anki.model            = ss( model_->currentText() );
		anki.allow_duplicates = duplicates_->isChecked();
		anki.tags.clear();
		for ( const QString& tag : tags_->text().split( ' ', Qt::SkipEmptyParts ) )
		{
			anki.tags.push_back( ss( tag ) );
		}
		anki.fields.clear();
		for ( int row = 0; row < fields_->rowCount(); ++row )
		{
			const auto* name  = fields_->item( row, 0 );
			const auto* value = qobject_cast<QComboBox*>( fields_->cellWidget( row, 1 ) );
			if ( name != nullptr && value != nullptr )
			{
				anki.fields.push_back( { .name = ss( name->text() ), .value = ss( value->currentText() ) } );
			}
		}
		settings().commit();
	}

	void AnkiPage::request( const QString& action, const QJsonObject& params, Reply done )
	{
		QJsonObject body{ { QStringLiteral( "action" ), action }, { QStringLiteral( "version" ), 6 }, { QStringLiteral( "params" ), params } };
		if ( !key_->text().isEmpty() )
		{
			body.insert( QStringLiteral( "key" ), key_->text() );
		}
		QNetworkRequest http( QUrl( url_->text().trimmed().isEmpty() ? QStringLiteral( "http://127.0.0.1:8765" ) : url_->text().trimmed() ) );
		http.setHeader( QNetworkRequest::ContentTypeHeader, QStringLiteral( "application/json" ) );
		http.setTransferTimeout( 5000 );
		QNetworkReply* reply = network_->post( http, QJsonDocument( body ).toJson( QJsonDocument::Compact ) );
		connect( reply, &QNetworkReply::finished, this, [reply, done = std::move( done )] {
			reply->deleteLater();
			if ( reply->error() != QNetworkReply::NoError )
			{
				done( {}, QStringLiteral( "AnkiConnect is not reachable (is Anki running with the AnkiConnect add-on?)" ) );
				return;
			}
			const auto document = QJsonDocument::fromJson( reply->readAll() );
			const auto error    = document.object().value( QStringLiteral( "error" ) );
			if ( error.isString() )
			{
				done( {}, error.toString() );
				return;
			}
			done( document.object().value( QStringLiteral( "result" ) ), {} );
		} );
	}

	void AnkiPage::connectAnki()
	{
		anki_status_->setText( QStringLiteral( "Connecting…" ) );
		request( QStringLiteral( "version" ), {}, [this]( const QJsonValue& version, const QString& error ) {
			anki_connected_ = error.isEmpty();
			if ( !error.isEmpty() )
			{
				anki_status_->setText( error );
				return;
			}
			anki_status_->setText( QStringLiteral( "Connected to AnkiConnect (version %1)." ).arg( version.toInt() ) );
			const auto fill = [this]( QComboBox* combo, const QJsonValue& names ) {
				loading_              = true;
				const QString current = combo->currentText();
				combo->clear();
				for ( const auto name : names.toArray() )
				{
					combo->addItem( name.toString() );
				}
				if ( combo->findText( current ) >= 0 )
				{
					combo->setCurrentText( current );
				}
				loading_ = false;
			};
			request( QStringLiteral( "deckNames" ), {}, [this, fill]( const QJsonValue& decks, const QString& ) {
				fill( deck_, decks );
				storeAnki();
			} );
			request( QStringLiteral( "modelNames" ), {}, [this, fill]( const QJsonValue& models, const QString& ) {
				const QString before = model_->currentText();
				fill( model_, models );
				if ( model_->currentText() != before || fields_->rowCount() == 0 )
				{
					loadFields( model_->currentText() );
				}
				else
				{
					storeAnki();
				}
			} );
		} );
	}

	void AnkiPage::loadFields( const QString& model )
	{
		request( QStringLiteral( "modelFieldNames" ), { { QStringLiteral( "modelName" ), model } }, [this]( const QJsonValue& names, const QString& error ) {
			if ( !error.isEmpty() )
			{
				anki_status_->setText( error );
				return;
			}
			QStringList list;
			for ( const auto name : names.toArray() )
			{
				list << name.toString();
			}
			showFields( list );
			storeAnki();
		} );
	}

	void AnkiPage::showFields( const QStringList& names )
	{
		const auto& saved = settings().config().anki.fields;
		QStringList markers{ QString() };
		for ( const auto marker : config::anki_markers )
		{
			markers << QStringLiteral( "{%1}" ).arg( qs( marker ) );
		}
		markers << QStringLiteral( "{cloze-prefix}<b>{cloze-body}</b>{cloze-suffix}" );

		fields_->setRowCount( static_cast<int>( names.size() ) );
		for ( int row = 0; row < names.size(); ++row )
		{
			auto* name = new QTableWidgetItem( names[row] );
			name->setFlags( Qt::ItemIsEnabled );
			fields_->setItem( row, 0, name );

			auto* value = new QComboBox();
			value->setEditable( true );
			value->addItems( markers );
			const auto it = std::ranges::find( saved, ss( names[row] ), &config::AnkiField::name );
			value->setCurrentText( it != saved.end() ? qs( it->value ) : guessTemplate( names[row], row == 0 ) );
			connect( value, &QComboBox::currentTextChanged, this, [this] { storeAnki(); } );
			fields_->setCellWidget( row, 1, value );
		}
	}

} // namespace lexiglance::gui
