#include "SetupWizard.h"

#include "DaemonClient.h"
#include "OcrInstall.h"
#include "Settings.h"
#include "TranslationInstall.h"
#include "VcRedist.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <span>

namespace lexiglance::gui
{

	namespace
	{

		QString downloadPath( const QString& name )
		{
			QString safe = name;
			safe.replace( QRegularExpression( QStringLiteral( "[^A-Za-z0-9._-]" ) ), QStringLiteral( "_" ) );
			return qs( ( paths::cacheDir() / "downloads" ).string() ) + "/" + safe + ".zip";
		}

		struct DictJob
		{
			QString name;
			QUrl    url;
		};

		std::vector<DictJob> dictionaryJobs( std::span<const lang::Language* const> languages )
		{
			std::vector<DictJob> jobs;
			for ( const lang::Language* language : languages )
			{
				for ( const lang::Recommendation& item : language->recommendedDictionaries() )
				{
					if ( item.download.empty() )
					{
						continue;
					}
					const QString name = qs( item.name );
					if ( std::ranges::any_of( jobs, [&]( const DictJob& job ) { return job.name == name; } ) )
					{
						continue;
					}
					jobs.push_back( { .name = name, .url = QUrl( qs( item.download ) ) } );
				}
			}
			return jobs;
		}

		// A line of the welcome: an icon, what Lexiglance does in bold, and a sentence about it.
		QWidget* feature( const QString& icon, const QString& title, const QString& text )
		{
			auto* row    = new QWidget();
			auto* layout = new QHBoxLayout( row );
			layout->setContentsMargins( 0, 0, 0, 0 );
			layout->setSpacing( 14 );
			auto* picture = new QLabel();
			picture->setPixmap( QIcon::fromTheme( icon ).pixmap( 26, 26 ) );
			picture->setAlignment( Qt::AlignTop | Qt::AlignHCenter );
			picture->setFixedWidth( 30 );
			layout->addWidget( picture );
			auto* words = new QLabel( QStringLiteral( "<b>%1</b><br>%2" ).arg( title.toHtmlEscaped(), text.toHtmlEscaped() ) );
			words->setTextFormat( Qt::RichText );
			words->setWordWrap( true );
			layout->addWidget( words, 1 );
			return row;
		}

		// "Japanese, Russian and Greek".
		QString listed( const QStringList& names )
		{
			if ( names.size() < 2 )
			{
				return names.join( QString() );
			}
			return QStringLiteral( "%1 and %2" ).arg( names.mid( 0, names.size() - 1 ).join( QStringLiteral( ", " ) ), names.back() );
		}

		// The first words of a language's sample, to show beside its name.
		QString sampleOf( const lang::Language& language )
		{
			const QString text = qs( language.sampleText() );
			if ( text.size() <= 30 )
			{
				return text;
			}
			const auto space = text.lastIndexOf( QLatin1Char( ' ' ), 30 );
			return text.left( space > 10 ? space : 30 ).remove( QRegularExpression( QStringLiteral( "[,、]$" ) ) ) + QStringLiteral( "…" );
		}

		void styleBar( QProgressBar* bar )
		{
			bar->setTextVisible( true );
			bar->setMinimumHeight( 18 );
			bar->setFormat( QStringLiteral( "%p%" ) );
		}

		void applyByteProgress( QProgressBar* bar, const std::vector<std::pair<qint64, qint64>>& sizes )
		{
			qint64 done  = 0;
			qint64 all   = 0;
			bool   known = true;
			for ( const auto& [received, total] : sizes )
			{
				done += received;
				all += total;
				known = known && total > 0;
			}
			showProgress( bar, done, known ? all : 0 );
		}

	} // namespace

	struct SetupWizard::InstallBatch
	{
		std::vector<DictJob>                                    dicts;
		std::vector<std::pair<QUrl, QString>>                   ocr;
		std::deque<std::pair<QString, QString>>                 import_queue; // name, local path
		std::shared_ptr<std::vector<std::pair<qint64, qint64>>> dict_sizes;
		std::shared_ptr<std::vector<std::pair<qint64, qint64>>> ocr_sizes;
		std::size_t                                             dict_downloads_left = 0;
		std::size_t                                             dict_imports_left   = 0;
		std::size_t                                             ocr_downloads_left  = 0;
		std::size_t                                             dict_finished       = 0;
		bool                                                    importing           = false;
		bool                                                    unpack_runtime      = false;
		bool                                                    finishing           = false;
		QString                                                 failure;
		QString                                                 runtime;
		QString                                                 archive;
	};

	SetupWizard::SetupWizard( Context context, std::function<void()> finished, QWidget* parent ) :
		QWidget( parent ),
		context_( std::move( context ) ),
		finished_( std::move( finished ) ),
		downloader_( new Downloader( this ) ),
		card_( new QFrame( this ) ),
		pages_( new QStackedWidget() ),
		welcome_title_( new QLabel() ),
		lock_hint_( new QLabel() ),
		continue_( new QPushButton( QStringLiteral( "Continue" ) ) ),
		skip_( new QPushButton( QStringLiteral( "Skip for now" ) ) ),
		install_status_( new QLabel() ),
		overall_label_( new QLabel( QStringLiteral( "Overall" ) ) ),
		overall_bar_( new QProgressBar() ),
		dict_label_( new QLabel( QStringLiteral( "Dictionaries" ) ) ),
		dict_bar_( new QProgressBar() ),
		ocr_label_( new QLabel( QStringLiteral( "OCR and runtime" ) ) ),
		ocr_bar_( new QProgressBar() ),
		translation_box_( new QCheckBox( QStringLiteral( "Translate sentences into English too" ) ) ),
		translation_note_( new QLabel() ),
		tips_( new QLabel() ),
		done_blurb_( new QLabel() ),
		practice_( new QTextBrowser() ),
		practice_status_( new QLabel() ),
		done_( new QPushButton( QStringLiteral( "Get started" ) ) )
	{
		hide();
		setFocusPolicy( Qt::StrongFocus );
		card_->setObjectName( QStringLiteral( "helpCard" ) );

		auto* card_layout = new QVBoxLayout( card_ );
		card_layout->setContentsMargins( 32, 28, 32, 24 );
		card_layout->setSpacing( 14 );
		card_layout->addWidget( pages_ );

		QFont title_font = welcome_title_->font();
		title_font.setPointSizeF( title_font.pointSizeF() * 1.55 );
		title_font.setBold( true );

		// --- Welcome ---
		auto* welcome        = new QWidget();
		auto* welcome_layout = new QVBoxLayout( welcome );
		welcome_layout->setContentsMargins( 0, 0, 0, 0 );
		welcome_layout->setSpacing( 10 );

		auto* logo = new QLabel();
		logo->setAlignment( Qt::AlignHCenter );
		const QPixmap mark( QStringLiteral( ":/lexiglance/lexiglance.png" ) );
		logo->setPixmap( mark.scaled( 72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
		welcome_layout->addWidget( logo );

		welcome_title_->setFont( title_font );
		welcome_title_->setText( QStringLiteral( "Welcome to Lexiglance" ) );
		welcome_title_->setAlignment( Qt::AlignHCenter );
		welcome_title_->setWordWrap( true );
		welcome_layout->addWidget( welcome_title_ );

		auto* blurb = new QLabel( QStringLiteral( "A pop-up dictionary for reading in the languages you learn, in any program." ) );
		blurb->setAlignment( Qt::AlignHCenter );
		blurb->setWordWrap( true );
		welcome_layout->addWidget( blurb );

		QStringList offered;
		for ( const lang::Language* language : lang::languages() )
		{
			if ( !language->recommendedDictionaries().empty() )
			{
				offered << qs( language->name() );
			}
		}
		auto* features = new QVBoxLayout();
		features->setContentsMargins( 20, 14, 20, 4 );
		features->setSpacing( 12 );
		features->addWidget( feature( QStringLiteral( "edit-find" ), QStringLiteral( "Point at a word" ), QStringLiteral( "Hold a key over text anywhere: a browser, a chat, a game, a video. What it means appears beside it." ) ) );
		features->addWidget( feature( QStringLiteral( "preferences-desktop-locale" ), QStringLiteral( "Whole sentences in English" ), QStringLiteral( "Hold one more key and the sentence is translated, on this computer." ) ) );
		features->addWidget( feature( QStringLiteral( "accessories-dictionary" ), QStringLiteral( "Dictionaries ready to use" ), QStringLiteral( "For %1, downloaded in a moment, with what reads text in games and videos." ).arg( listed( offered ) ) ) );
		features->addWidget( feature( QStringLiteral( "document-send" ), QStringLiteral( "Keep what you learn" ), QStringLiteral( "Send words to Anki, with the sentence they came from." ) ) );
		welcome_layout->addLayout( features );

		lock_hint_->setWordWrap( true );
		lock_hint_->setAlignment( Qt::AlignHCenter );
		lock_hint_->setEnabled( false );
		welcome_layout->addWidget( lock_hint_ );
		welcome_layout->addStretch( 1 );
		auto* welcome_footer = new QHBoxLayout();
		welcome_footer->addWidget( skip_ );
		welcome_footer->addStretch( 1 );
		continue_->setProperty( "primary", true );
		welcome_footer->addWidget( continue_ );
		welcome_layout->addLayout( welcome_footer );
		pages_->addWidget( welcome );

		// --- Languages ---
		auto* languages_page   = new QWidget();
		auto* languages_layout = new QVBoxLayout( languages_page );
		languages_layout->setContentsMargins( 0, 0, 0, 0 );
		auto* languages_title = new QLabel( QStringLiteral( "Which languages do you read?" ) );
		languages_title->setFont( title_font );
		languages_title->setWordWrap( true );
		languages_layout->addWidget( languages_title );
		auto* languages_blurb = new QLabel( QStringLiteral( "The recommended dictionaries of each, and what reads its text from the screen, are downloaded next. "
		                                                    "Languages can be added or turned off later on the Scanning page." ) );
		languages_blurb->setWordWrap( true );
		languages_layout->addWidget( languages_blurb );

		auto* scroll = new QScrollArea();
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		// The card's own colour behind the list.
		auto* list_host = new QWidget();
		list_host->setObjectName( QStringLiteral( "setupLanguages" ) );
		scroll->setStyleSheet( QStringLiteral( "QScrollArea, QScrollArea > QWidget, QWidget#setupLanguages { background: transparent; }" ) );
		auto* list_layout = new QVBoxLayout( list_host );
		list_layout->setContentsMargins( 0, 10, 0, 10 );
		list_layout->setSpacing( 10 );
		for ( const lang::Language* language : lang::languages() )
		{
			if ( language->recommendedDictionaries().empty() )
			{
				continue;
			}
			auto* row    = new QHBoxLayout();
			auto* box    = new QCheckBox( qs( language->name() ) );
			auto* sample = new QLabel( sampleOf( *language ) );
			sample->setEnabled( false );
			box->setMinimumWidth( 120 );
			row->addWidget( box );
			row->addWidget( sample, 1 );
			language_boxes_.push_back( box );
			language_codes_.emplace_back( language->code() );
			list_layout->addLayout( row );
		}
		list_layout->addStretch( 1 );
		scroll->setWidget( list_host );
		languages_layout->addWidget( scroll, 1 );
		translation_box_->setChecked( true );
		languages_layout->addWidget( translation_box_ );
		translation_note_->setWordWrap( true );
		translation_note_->setEnabled( false );
		translation_note_->setContentsMargins( 26, 0, 0, 6 );
		languages_layout->addWidget( translation_note_ );

		auto* languages_footer = new QHBoxLayout();
		auto* back             = new QPushButton( QStringLiteral( "Back" ) );
		auto* install          = new QPushButton( QStringLiteral( "Install and continue" ) );
		install->setProperty( "primary", true );
		languages_footer->addWidget( back );
		languages_footer->addStretch( 1 );
		languages_footer->addWidget( install );
		languages_layout->addLayout( languages_footer );
		pages_->addWidget( languages_page );

		// --- Install ---
		auto* install_page   = new QWidget();
		auto* install_layout = new QVBoxLayout( install_page );
		install_layout->setContentsMargins( 0, 0, 0, 0 );
		install_layout->setSpacing( 10 );
		auto* install_title = new QLabel( QStringLiteral( "Setting up" ) );
		install_title->setFont( title_font );
		install_layout->addWidget( install_title );
		install_status_->setWordWrap( true );
		install_status_->setText( QStringLiteral( "Preparing downloads..." ) );
		install_layout->addWidget( install_status_ );

		styleBar( overall_bar_ );
		styleBar( dict_bar_ );
		styleBar( ocr_bar_ );
		overall_label_->setEnabled( false );
		dict_label_->setEnabled( false );
		ocr_label_->setEnabled( false );
		install_layout->addWidget( overall_label_ );
		install_layout->addWidget( overall_bar_ );
		install_layout->addWidget( dict_label_ );
		install_layout->addWidget( dict_bar_ );
		install_layout->addWidget( ocr_label_ );
		install_layout->addWidget( ocr_bar_ );

		// While it downloads: how Lexiglance is used.
		tips_->setWordWrap( true );
		tips_->setTextFormat( Qt::RichText );
		install_layout->addSpacing( 12 );
		install_layout->addWidget( tips_ );

		auto* install_continue = new QPushButton( QStringLiteral( "Continue anyway" ) );
		install_continue->setObjectName( QStringLiteral( "setupContinueAnyway" ) );
		install_continue->setProperty( "primary", true );
		install_continue->hide();
		auto* install_footer = new QHBoxLayout();
		install_footer->addStretch( 1 );
		install_footer->addWidget( install_continue );
		install_layout->addStretch( 1 );
		install_layout->addLayout( install_footer );
		pages_->addWidget( install_page );
		connect( install_continue, &QPushButton::clicked, this, [this] { showPage( Page::Done ); } );

		// --- Done ---
		auto* done_page   = new QWidget();
		auto* done_layout = new QVBoxLayout( done_page );
		done_layout->setContentsMargins( 0, 0, 0, 0 );
		auto* done_logo = new QLabel();
		done_logo->setAlignment( Qt::AlignHCenter );
		done_logo->setPixmap( mark.scaled( 56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
		done_layout->addWidget( done_logo );
		auto* done_title = new QLabel( QStringLiteral( "Try it now" ) );
		done_title->setFont( title_font );
		done_title->setAlignment( Qt::AlignHCenter );
		done_layout->addWidget( done_title );
		done_blurb_->setWordWrap( true );
		done_blurb_->setAlignment( Qt::AlignHCenter );
		done_blurb_->setTextFormat( Qt::RichText );
		done_layout->addWidget( done_blurb_ );
		// A sentence to point at, in the language chosen first: text the daemon reads like any other program's.
		QFont practice_font = practice_->font();
		practice_font.setPointSizeF( practice_font.pointSizeF() * 1.7 );
		practice_->setFont( practice_font );
		practice_->setFrameShape( QFrame::NoFrame );
		practice_->setStyleSheet( QStringLiteral( "QTextBrowser { border: none; border-radius: 10px; background: rgba(127, 127, 127, 0.10); }" ) );
		practice_->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		practice_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		practice_->setFocusPolicy( Qt::NoFocus );
		practice_->document()->setDocumentMargin( 14 );
		// However the sentence comes to be laid out (a longer one wraps), the box is as tall as it.
		connect( practice_->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { fitPractice(); } );
		done_layout->addSpacing( 10 );
		done_layout->addWidget( practice_ );
		practice_status_->setWordWrap( true );
		practice_status_->setAlignment( Qt::AlignHCenter );
		practice_status_->setTextFormat( Qt::RichText );
		done_layout->addWidget( practice_status_ );
		done_layout->addStretch( 1 );
		auto* later = new QLabel( QStringLiteral( "Everything else is explained in Help, at the bottom of the sidebar." ) );
		later->setAlignment( Qt::AlignHCenter );
		later->setEnabled( false );
		done_layout->addWidget( later );
		auto* done_footer = new QHBoxLayout();
		done_footer->addStretch( 1 );
		done_->setProperty( "primary", true );
		done_footer->addWidget( done_ );
		done_layout->addLayout( done_footer );
		pages_->addWidget( done_page );

		connect( continue_, &QPushButton::clicked, this, [this] { showPage( Page::Languages ); } );
		connect( skip_, &QPushButton::clicked, this, [this] { complete(); } );
		connect( back, &QPushButton::clicked, this, [this] { showPage( Page::Welcome ); } );
		connect( install, &QPushButton::clicked, this, [this] {
			if ( selectedLanguages().empty() )
			{
				return;
			}
			applyLanguages();
			startInstall();
		} );
		connect( done_, &QPushButton::clicked, this, [this] { complete(); } );

		for ( QCheckBox* box : language_boxes_ )
		{
			connect( box, &QCheckBox::toggled, this, [this]( bool on ) {
				updateTranslationBox();
				updatePractice();
				if ( on )
				{
					return;
				}
				if ( std::ranges::none_of( language_boxes_, []( QCheckBox* other ) { return other->isChecked(); } ) )
				{
					QObject::sender()->blockSignals( true );
					qobject_cast<QCheckBox*>( QObject::sender() )->setChecked( true );
					QObject::sender()->blockSignals( false );
				}
			} );
		}

		// The sentence to try answers the keys: what the daemon did with them shows under it.
		context_.client->onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( !isVisible() || pages_->currentIndex() != static_cast<int>( Page::Done ) )
			{
				return;
			}
			if ( name == "trigger.changed" && params["held"].asBool() && !tried_ )
			{
				practice_status_->setText( QStringLiteral( "Now point at a word of the sentence." ) );
			}
			else if ( name == "capture.result" && params["found"].asBool() && params["entries"].asInt() > 0 )
			{
				tried_ = true;
				practice_status_->setText( QStringLiteral( "<span style=\"color:#3fa45b; font-weight:600\">✓ It works.</span> It does the same in every program." ) );
			}
			else if ( name == "capture.result" && !tried_ )
			{
				practice_status_->setText( QStringLiteral( "<span style=\"color:#d19a1f\">Nothing found there</span>: point at the middle of a word." ) );
			}
		} );

		parent->installEventFilter( this );
		practice_->installEventFilter( this );
	}

	void SetupWizard::open( int lock_seconds, bool reinstall, bool required )
	{
		reinstall_    = reinstall;
		required_     = required;
		unlock_at_ms_ = lock_seconds > 0 ? QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>( lock_seconds ) * 1000 : 0;
		installing_   = false;
		resetProgress();
		if ( auto* button = findChild<QPushButton*>( QStringLiteral( "setupContinueAnyway" ) ) )
		{
			button->hide();
		}
		skip_->setVisible( !required_ );
		const auto& cfg       = context_.settings->config();
		const bool  first_run = !applicationMemory().value( QStringLiteral( "setup/completed" ) ).toBool();
		for ( std::size_t i = 0; i < language_boxes_.size(); ++i )
		{
			if ( first_run )
			{
				language_boxes_[i]->setChecked( language_codes_[i] == cfg.language || ( cfg.language.empty() && language_codes_[i] == "ja" ) );
			}
			else
			{
				language_boxes_[i]->setChecked( !std::ranges::contains( cfg.disabled_languages, language_codes_[i] ) );
			}
		}
		if ( !language_boxes_.empty() && std::ranges::none_of( language_boxes_, []( QCheckBox* box ) { return box->isChecked(); } ) )
		{
			language_boxes_.front()->setChecked( true );
		}
		updateTranslationBox();
		updateKeys();
		updatePractice();
		showPage( Page::Welcome );
		if ( locked() )
		{
			lock_hint_->setText(
					required_ ? QStringLiteral( "This card stays for a few seconds so setup is not closed by accident. Setup cannot be skipped." )
							  : QStringLiteral( "This card stays for a few seconds so setup is not closed by accident." )
			);
			lock_hint_->show();
			continue_->setEnabled( false );
			skip_->setEnabled( false );
			const int remaining = static_cast<int>( unlock_at_ms_ - QDateTime::currentMSecsSinceEpoch() );
			QTimer::singleShot( std::max( 0, remaining ), this, [this] {
				lock_hint_->hide();
				continue_->setEnabled( true );
				skip_->setEnabled( true );
				continue_->setFocus();
			} );
		}
		else
		{
			lock_hint_->hide();
			continue_->setEnabled( true );
			skip_->setEnabled( true );
		}
		place();
		show();
		raise();
		if ( !locked() )
		{
			continue_->setFocus();
		}
	}

	void SetupWizard::resetProgress()
	{
		overall_bar_->show();
		dict_bar_->show();
		ocr_bar_->show();
		overall_label_->show();
		dict_label_->show();
		ocr_label_->show();
		setOverall( 0, 1, QStringLiteral( "Overall" ) );
		dict_label_->setText( QStringLiteral( "Dictionaries" ) );
		dict_label_->setEnabled( false );
		dict_bar_->setRange( 0, 1 );
		dict_bar_->setValue( 0 );
		dict_bar_->setFormat( QStringLiteral( "%p%" ) );
		ocr_label_->setText( QStringLiteral( "OCR and runtime" ) );
		ocr_label_->setEnabled( false );
		ocr_bar_->setRange( 0, 1 );
		ocr_bar_->setValue( 0 );
		ocr_bar_->setFormat( QStringLiteral( "%p%" ) );
		install_status_->setText( QStringLiteral( "Preparing downloads..." ) );
	}

	void SetupWizard::setOverall( int value, int maximum, const QString& text )
	{
		overall_label_->setText( text );
		overall_label_->setEnabled( true );
		overall_bar_->setRange( 0, std::max( 1, maximum ) );
		overall_bar_->setValue( std::clamp( value, 0, overall_bar_->maximum() ) );
		overall_bar_->setFormat( QStringLiteral( "%v / %m" ) );
	}

	void SetupWizard::place()
	{
		setGeometry( parentWidget()->rect() );
		card_->ensurePolished();
		const int width  = std::min( 580, this->width() - 48 );
		const int height = std::min( 560, this->height() - 48 );
		card_->setGeometry( ( this->width() - width ) / 2, ( this->height() - height ) / 2, width, height );
	}

	bool SetupWizard::eventFilter( QObject* watched, QEvent* event )
	{
		if ( watched == parentWidget() && event->type() == QEvent::Resize && isVisible() )
		{
			place();
		}
		if ( watched == practice_ && event->type() == QEvent::Resize )
		{
			fitPractice();
		}
		return QWidget::eventFilter( watched, event );
	}

	void SetupWizard::paintEvent( QPaintEvent* /*event*/ )
	{
		QPainter painter( this );
		painter.fillRect( rect(), QColor( 0, 0, 0, 110 ) );
	}

	void SetupWizard::mousePressEvent( QMouseEvent* event )
	{
		if ( !dismissible() || pages_->currentIndex() == static_cast<int>( Page::Install ) )
		{
			return;
		}
		if ( !card_->geometry().contains( event->position().toPoint() ) )
		{
			complete();
		}
	}

	void SetupWizard::keyPressEvent( QKeyEvent* event )
	{
		if ( event->key() == Qt::Key_Escape )
		{
			if ( dismissible() && pages_->currentIndex() != static_cast<int>( Page::Install ) )
			{
				complete();
			}
			return;
		}
		QWidget::keyPressEvent( event );
	}

	bool SetupWizard::locked() const
	{
		return unlock_at_ms_ > 0 && QDateTime::currentMSecsSinceEpoch() < unlock_at_ms_;
	}

	bool SetupWizard::dismissible() const
	{
		return !required_ && !locked() && !installing_;
	}

	void SetupWizard::showPage( Page page )
	{
		pages_->setCurrentIndex( static_cast<int>( page ) );
		place();
		if ( page == Page::Done )
		{
			updatePractice();
		}
	}

	std::vector<const lang::Language*> SetupWizard::selectedLanguages() const
	{
		std::vector<const lang::Language*> out;
		for ( std::size_t i = 0; i < language_boxes_.size(); ++i )
		{
			if ( language_boxes_[i]->isChecked() )
			{
				if ( const lang::Language* language = lang::findLanguage( language_codes_[i] ) )
				{
					out.push_back( language );
				}
			}
		}
		return out;
	}

	void SetupWizard::applyLanguages()
	{
		std::vector<std::string> disabled;
		std::string              first;
		for ( std::size_t i = 0; i < language_boxes_.size(); ++i )
		{
			if ( language_boxes_[i]->isChecked() )
			{
				if ( first.empty() )
				{
					first = language_codes_[i];
				}
			}
			else
			{
				disabled.push_back( language_codes_[i] );
			}
		}
		auto&                          config              = context_.settings->config();
		const std::vector<std::string> previously_disabled = config.disabled_languages;
		for ( const lang::Language* language : lang::languages() )
		{
			const std::string code( language->code() );
			if ( language->recommendedDictionaries().empty() )
			{
				// Not offered in the wizard: keep the user's previous on/off choice.
				if ( std::ranges::contains( previously_disabled, code ) )
				{
					disabled.push_back( code );
				}
				continue;
			}
			if ( !std::ranges::contains( language_codes_, code ) )
			{
				disabled.push_back( code );
			}
		}
		config.disabled_languages = std::move( disabled );
		if ( !first.empty() )
		{
			config.language = first;
		}
		if ( config.scan.ocr == config::OcrMode::Off )
		{
			config.scan.ocr = config::OcrMode::Fallback;
		}
		context_.settings->commit();
	}

	void SetupWizard::startInstall()
	{
		installing_ = true;
		showPage( Page::Install );
		resetProgress();
		install_status_->setText( QStringLiteral( "Preparing downloads..." ) );

		context_.client->call( "dictionaries.list", "{}", [this]( const json::Value* result, const QString& ) {
			std::vector<QString> installed_titles;
			if ( result != nullptr )
			{
				for ( const json::Value& item : ( *result )["dictionaries"].items() )
				{
					installed_titles.push_back( qs( item["title"].asString() ) );
				}
			}

			auto batch   = std::make_shared<InstallBatch>();
			batch->dicts = dictionaryJobs( selectedLanguages() );
			if ( !reinstall_ )
			{
				std::erase_if( batch->dicts, [&]( const DictJob& job ) {
					for ( const lang::Language* language : selectedLanguages() )
					{
						for ( const lang::Recommendation& item : language->recommendedDictionaries() )
						{
							if ( qs( item.name ) != job.name )
							{
								continue;
							}
							return std::ranges::any_of( installed_titles, [&]( const QString& title ) {
								return ( !item.title.empty() && title == qs( item.title ) ) || ( !item.title_prefix.empty() && title.startsWith( qs( item.title_prefix ) ) );
							} );
						}
					}
					return false;
				} );
			}

			batch->ocr = ocr_install::paddleFiles( selectedLanguages() );
			batch->ocr = reinstall_ ? ocr_install::toDownload( std::move( batch->ocr ) ) : ocr_install::missingOnly( std::move( batch->ocr ) );
			// The translation models come with them, over the same runtime.
			if ( translation_box_->isChecked() )
			{
				std::ranges::move( translation_install::files( translation_install::wanted( selectedLanguages(), context_.settings->config().translation ), reinstall_ ), std::back_inserter( batch->ocr ) );
			}
			batch->runtime = ocr_install::ocrPath( "runtime" );
			batch->archive = ocr_install::runtimeArchive();
			redist_.clear();
			ocr_install::appendRuntime( batch->ocr, &redist_, this, false, reinstall_ );
			const QString packed  = ocr_install::runtimePackedPath();
			batch->unpack_runtime = !packed.isEmpty() && std::ranges::any_of( batch->ocr, [&]( const auto& file ) { return file.second == packed; } );
			beginBatch( batch );
		} );
	}

	void SetupWizard::beginBatch( const std::shared_ptr<InstallBatch>& batch )
	{
		batch->dict_downloads_left = batch->dicts.size();
		batch->dict_imports_left   = batch->dicts.size();
		batch->ocr_downloads_left  = batch->ocr.size();
		batch->dict_finished       = 0;
		batch->dict_sizes          = std::make_shared<std::vector<std::pair<qint64, qint64>>>( batch->dicts.size() );
		batch->ocr_sizes           = std::make_shared<std::vector<std::pair<qint64, qint64>>>( batch->ocr.size() );

		dict_label_->setEnabled( true );
		ocr_label_->setEnabled( true );
		if ( batch->dicts.empty() )
		{
			dict_label_->setText( QStringLiteral( "Dictionaries (already installed)" ) );
			dict_bar_->setRange( 0, 1 );
			dict_bar_->setValue( 1 );
		}
		else
		{
			dict_label_->setText( QStringLiteral( "Dictionaries" ) );
			dict_bar_->setRange( 0, 0 );
		}
		const QString models = translation_box_->isChecked() ? QStringLiteral( "OCR, translation and runtime" ) : QStringLiteral( "OCR and runtime" );
		if ( batch->ocr.empty() )
		{
			ocr_label_->setText( models + QStringLiteral( " (already installed)" ) );
			ocr_bar_->setRange( 0, 1 );
			ocr_bar_->setValue( 1 );
		}
		else
		{
			ocr_label_->setText( models );
			ocr_bar_->setRange( 0, 0 );
		}

		const int overall_max = static_cast<int>( batch->dicts.size() + batch->ocr.size() );
		setOverall( 0, std::max( 1, overall_max ), QStringLiteral( "Overall" ) );
		if ( batch->dicts.empty() && batch->ocr.empty() )
		{
			setOverall( 1, 1, QStringLiteral( "Overall" ) );
			finishInstall( {} );
			return;
		}

		install_status_->setText( translation_box_->isChecked() ? QStringLiteral( "Downloading dictionaries, OCR and translation models, and runtime together..." ) : QStringLiteral( "Downloading dictionaries, OCR models, and runtime together..." ) );

		for ( std::size_t i = 0; i < batch->dicts.size(); ++i )
		{
			const DictJob job    = batch->dicts[i];
			const QString target = downloadPath( job.name );
			downloader_->download(
					job.url,
					target,
					[this, batch, i]( qint64 received, qint64 total ) {
						( *batch->dict_sizes )[i] = { received, total };
						refreshProgress( batch );
					},
					[this, batch, job, target]( const QString& error ) { onDictDownloadDone( batch, job.name, target, error ); }
			);
		}

		for ( std::size_t i = 0; i < batch->ocr.size(); ++i )
		{
			downloader_->download(
					batch->ocr[i].first,
					batch->ocr[i].second,
					[this, batch, i]( qint64 received, qint64 total ) {
						( *batch->ocr_sizes )[i] = { received, total };
						refreshProgress( batch );
					},
					[this, batch]( const QString& error ) { onOcrDownloadDone( batch, error ); }
			);
		}
	}

	void SetupWizard::refreshProgress( const std::shared_ptr<InstallBatch>& batch )
	{
		if ( !batch->dicts.empty() )
		{
			applyByteProgress( dict_bar_, *batch->dict_sizes );
		}
		if ( !batch->ocr.empty() )
		{
			applyByteProgress( ocr_bar_, *batch->ocr_sizes );
		}
		const int done = static_cast<int>( batch->dict_finished + ( batch->ocr.size() - batch->ocr_downloads_left ) );
		const int all  = static_cast<int>( batch->dicts.size() + batch->ocr.size() );
		if ( all > 0 )
		{
			setOverall( done, all, QStringLiteral( "Overall" ) );
		}
	}

	void SetupWizard::onDictDownloadDone( const std::shared_ptr<InstallBatch>& batch, const QString& name, const QString& target, const QString& error )
	{
		if ( !error.isEmpty() && batch->failure.isEmpty() )
		{
			batch->failure = QStringLiteral( "%1: %2" ).arg( name, error );
		}
		if ( batch->dict_downloads_left > 0 )
		{
			--batch->dict_downloads_left;
		}
		if ( error.isEmpty() )
		{
			batch->import_queue.emplace_back( name, target );
		}
		else if ( batch->dict_imports_left > 0 )
		{
			--batch->dict_imports_left;
			++batch->dict_finished;
		}
		refreshProgress( batch );
		pumpImports( batch );
		tryFinishBatch( batch );
	}

	void SetupWizard::pumpImports( const std::shared_ptr<InstallBatch>& batch )
	{
		if ( batch->importing || batch->import_queue.empty() )
		{
			return;
		}
		batch->importing          = true;
		const auto [name, target] = batch->import_queue.front();
		batch->import_queue.pop_front();
		install_status_->setText( QStringLiteral( "Installing %1..." ).arg( name ) );
		dict_label_->setText( QStringLiteral( "Dictionaries: installing %1" ).arg( name ) );
		json::Writer params;
		params.beginObject().field( "path", ss( target ) ).field( "delete_source", true ).field( "replace", true ).endObject();
		context_.client->call( "dictionaries.import", params.take(), [this, batch, name]( const json::Value*, const QString& import_error ) {
			if ( !import_error.isEmpty() && batch->failure.isEmpty() )
			{
				batch->failure = QStringLiteral( "%1: %2" ).arg( name, import_error );
			}
			batch->importing = false;
			if ( batch->dict_imports_left > 0 )
			{
				--batch->dict_imports_left;
			}
			++batch->dict_finished;
			if ( !batch->dicts.empty() )
			{
				dict_bar_->setRange( 0, static_cast<int>( batch->dicts.size() ) );
				dict_bar_->setValue( static_cast<int>( batch->dict_finished ) );
				dict_bar_->setFormat( QStringLiteral( "%v / %m" ) );
			}
			refreshProgress( batch );
			pumpImports( batch );
			tryFinishBatch( batch );
		} );
	}

	void SetupWizard::onOcrDownloadDone( const std::shared_ptr<InstallBatch>& batch, const QString& error )
	{
		if ( !error.isEmpty() && batch->failure.isEmpty() )
		{
			batch->failure = error;
		}
		if ( batch->ocr_downloads_left > 0 )
		{
			--batch->ocr_downloads_left;
		}
		refreshProgress( batch );
		tryFinishBatch( batch );
	}

	void SetupWizard::tryFinishBatch( const std::shared_ptr<InstallBatch>& batch )
	{
		if ( batch->finishing || batch->dict_downloads_left > 0 || batch->dict_imports_left > 0 || batch->ocr_downloads_left > 0 || batch->importing )
		{
			return;
		}
		batch->finishing = true;

		const auto after_runtime = [this, batch]( const QString& unpack_error ) {
			if ( !unpack_error.isEmpty() )
			{
				if ( const QString path = std::exchange( redist_, QString() ); !path.isEmpty() )
				{
					QFile::remove( path );
				}
				finishInstall( unpack_error );
				return;
			}

			if ( !batch->dicts.empty() )
			{
				dict_bar_->setRange( 0, 1 );
				dict_bar_->setValue( 1 );
				dict_bar_->setFormat( QStringLiteral( "%p%" ) );
				dict_label_->setText( QStringLiteral( "Dictionaries" ) );
			}
			if ( !batch->ocr.empty() )
			{
				ocr_bar_->setRange( 0, 1 );
				ocr_bar_->setValue( 1 );
				ocr_bar_->setFormat( QStringLiteral( "%p%" ) );
				ocr_label_->setText( QStringLiteral( "OCR and runtime" ) );
			}
			setOverall( overall_bar_->maximum(), overall_bar_->maximum(), QStringLiteral( "Overall" ) );

			if ( !redist_.isEmpty() )
			{
				const QString installer = std::exchange( redist_, QString() );
				install_status_->setText( QStringLiteral( "Installing the Visual C++ Redistributable..." ) );
				vcredist::install( this, installer, [this]( const QString& redist_error ) {
					if ( !redist_error.isEmpty() )
					{
						finishInstall( redist_error );
						return;
					}
					context_.client->startDaemon( true );
					finishInstall( {} );
				} );
				return;
			}

			// Starting the daemon loads OCR with the new files; capture.reset only helps when it is already up.
			// call() fails immediately while the socket is still reconnecting after startDaemon / shutdown.
			if ( !context_.client->connected() )
			{
				context_.client->startDaemon( true );
				finishInstall( {} );
				return;
			}
			context_.client->call( "capture.reset", "{}", [this]( const json::Value*, const QString& reset_error ) {
				if ( !reset_error.isEmpty() )
				{
					log::warn( "setup: capture.reset: {}", ss( reset_error ) );
				}
				finishInstall( {} );
			} );
		};

		if ( batch->failure.isEmpty() && batch->unpack_runtime )
		{
			install_status_->setText( QStringLiteral( "Installing OCR runtime..." ) );
			const QString installed = batch->runtime + "/" + ocr_install::runtimeLibraryName();
			const auto    unpack    = [this, batch, after_runtime, installed] {
				QString error = ocr_install::unpackRuntime( batch->runtime, batch->archive );
				// Simulate / reinstall: if the daemon still held the library, keep the working copy already on disk.
				if ( !error.isEmpty() && reinstall_ && QFile::exists( installed ) )
				{
					log::warn( "setup: {} (keeping the ONNX Runtime already installed)", ss( error ) );
					error.clear();
				}
				after_runtime( error );
			};
			// The daemon keeps onnxruntime.dll mapped; replace it only after that process has gone.
			if ( QFile::exists( installed ) && context_.client->connected() )
			{
				context_.client->call( "shutdown", "{}", [this, unpack]( const json::Value*, const QString& ) {
					QTimer::singleShot( 900, this, unpack );
				} );
				return;
			}
			if ( QFile::exists( installed ) )
			{
				for ( const int pid : process::othersNamed( "lexiglanced" ) )
				{
					( void )process::terminate( pid, "lexiglanced", std::chrono::seconds( 2 ) );
				}
				QTimer::singleShot( 500, this, unpack );
				return;
			}
			unpack();
			return;
		}

		if ( !batch->failure.isEmpty() )
		{
			after_runtime( batch->failure );
			return;
		}
		after_runtime( {} );
	}

	void SetupWizard::updateTranslationBox()
	{
		const auto    chosen = selectedLanguages();
		const auto    size   = translation_install::bytes( translation_install::wanted( chosen, context_.settings->config().translation ) );
		const bool    any    = !translation_install::withModel( chosen ).empty();
		const QString label  = size > 0 ? QStringLiteral( "Translate sentences into English too (%1 more)" ).arg( translation_install::megabytes( size ) ) : QStringLiteral( "Translate sentences into English too" );
		translation_box_->setText( label );
		translation_box_->setEnabled( any );
		translation_note_->setVisible( any );
	}

	void SetupWizard::updateKeys()
	{
		const auto&   config   = context_.settings->config();
		const QString trigger  = chordText( config.scan.trigger ).toHtmlEscaped();
		const QString sentence = config.translation.sentence_key.empty() ? QString() : qs( config::displayName( config.translation.sentence_key ) ).toHtmlEscaped();
		translation_note_->setText( sentence.isEmpty() ? QStringLiteral( "Translated on this computer, nothing is sent anywhere. Faster full precision models are on the Translation page." ) : QStringLiteral( "Hold %1 with the trigger over a sentence to read it in English. Translated on this computer; "
		                                                                                                                                                                                                        "faster full precision models are on the Translation page." )
		                                                                                                                                                                                                .arg( sentence ) );
		QString    tips = QStringLiteral( "<b>Meanwhile, how it works</b><table cellspacing=\"0\" cellpadding=\"3\" style=\"margin-top:6px\">" );
		const auto tip  = [&]( const QString& what, const QString& how ) { tips += QStringLiteral( "<tr><td style=\"padding-right:14px\">%1</td><td>%2</td></tr>" ).arg( what, how ); };
		tip( QStringLiteral( "Look up a word" ), QStringLiteral( "Hold <b>%1</b> and point at it" ).arg( trigger ) );
		tip( QStringLiteral( "Select more characters" ), QStringLiteral( "Keep holding it and use the scroll wheel" ) );
		if ( !sentence.isEmpty() )
		{
			tip( QStringLiteral( "Translate the sentence" ), QStringLiteral( "Hold <b>%1</b> as well" ).arg( sentence ) );
		}
		tip( QStringLiteral( "In the popup" ), QStringLiteral( "Scroll for more entries, click one to copy it" ) );
		tips_->setText( tips + QStringLiteral( "</table>" ) );
		done_blurb_->setText( sentence.isEmpty() ? QStringLiteral( "Hold <b>%1</b> and point at a word below." ).arg( trigger ) : QStringLiteral( "Hold <b>%1</b> and point at a word below. Then add <b>%2</b> to read the sentence in English." ).arg( trigger, sentence ) );
	}

	void SetupWizard::updatePractice()
	{
		const auto chosen = selectedLanguages();
		tried_            = false;
		practice_status_->setText( QStringLiteral( "<span style=\"color:gray\">Keep holding the key and use the scroll wheel to select more characters.</span>" ) );
		practice_->setHtml( chosen.empty() ? QString() : QStringLiteral( "<p align=\"center\">%1</p>" ).arg( qs( chosen.front()->exampleSentence() ).toHtmlEscaped() ) );
		fitPractice();
	}

	void SetupWizard::fitPractice()
	{
		// As tall as the sentence, which wraps at the card's width.
		practice_->document()->setTextWidth( practice_->viewport()->width() );
		const int height = static_cast<int>( std::ceil( practice_->document()->size().height() ) ) + ( 2 * practice_->frameWidth() );
		if ( practice_->height() != height )
		{
			practice_->setFixedHeight( height );
		}
	}

	void SetupWizard::finishInstall( const QString& error )
	{
		installing_ = false;
		// Translation models that came are loaded when next needed.
		context_.client->call( "translation.reload" );
		auto* continue_anyway = findChild<QPushButton*>( QStringLiteral( "setupContinueAnyway" ) );
		if ( !error.isEmpty() )
		{
			log::warn( "setup: {}", ss( error ) );
			install_status_->setText( QStringLiteral( "Something could not be downloaded: %1\n\nYou can finish setup and install the rest from Dictionaries and Scanning." ).arg( error ) );
			if ( continue_anyway != nullptr )
			{
				continue_anyway->show();
			}
			return;
		}
		if ( continue_anyway != nullptr )
		{
			continue_anyway->hide();
		}
		showPage( Page::Done );
	}

	void SetupWizard::complete()
	{
		applicationMemory().setValue( QStringLiteral( "setup/completed" ), true );
		applicationMemory().setValue( QStringLiteral( "help/shown" ), true );
		hide();
		if ( finished_ )
		{
			finished_();
		}
	}

} // namespace lexiglance::gui
