#include "SetupWizard.h"

#include "DaemonClient.h"
#include "OcrInstall.h"
#include "Settings.h"
#include "VcRedist.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>

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

		QLabel* makeBullet( const QString& text )
		{
			auto* row = new QLabel( QStringLiteral( "<table cellspacing='0' cellpadding='0'><tr>"
			                                        "<td style='padding-right:10px;vertical-align:top;'>•</td>"
			                                        "<td>%1</td></tr></table>" )
			                                .arg( text.toHtmlEscaped() ) );
			row->setTextFormat( Qt::RichText );
			row->setWordWrap( true );
			row->setAlignment( Qt::AlignHCenter );
			return row;
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
		done_blurb_( new QLabel() ),
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

		auto* blurb = new QLabel( QStringLiteral( "A system-wide dictionary for reading in any program." ) );
		blurb->setAlignment( Qt::AlignHCenter );
		blurb->setWordWrap( true );
		welcome_layout->addWidget( blurb );

		auto* bullets = new QVBoxLayout();
		bullets->setContentsMargins( 24, 8, 24, 4 );
		bullets->setSpacing( 6 );
		bullets->addWidget( makeBullet( QStringLiteral( "Hold a key and point at a word anywhere on screen" ) ) );
		bullets->addWidget( makeBullet( QStringLiteral( "Dictionaries for Japanese, Russian, Chinese, and more" ) ) );
		bullets->addWidget( makeBullet( QStringLiteral( "Screen reading (OCR) for games, images, and video" ) ) );
		bullets->addWidget( makeBullet( QStringLiteral( "Optional export to Anki for spaced repetition" ) ) );
		welcome_layout->addLayout( bullets );

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
		auto* languages_blurb = new QLabel( QStringLiteral( "Lexiglance will download recommended dictionaries and OCR models for each one you select." ) );
		languages_blurb->setWordWrap( true );
		languages_layout->addWidget( languages_blurb );

		auto* scroll = new QScrollArea();
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		auto* list_host   = new QWidget();
		auto* list_layout = new QVBoxLayout( list_host );
		list_layout->setContentsMargins( 0, 8, 0, 8 );
		for ( const lang::Language* language : lang::languages() )
		{
			if ( language->recommendedDictionaries().empty() )
			{
				continue;
			}
			auto* box = new QCheckBox( qs( language->name() ) );
			language_boxes_.push_back( box );
			language_codes_.emplace_back( language->code() );
			list_layout->addWidget( box );
		}
		list_layout->addStretch( 1 );
		scroll->setWidget( list_host );
		languages_layout->addWidget( scroll, 1 );

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
		auto* done_title = new QLabel( QStringLiteral( "You are ready" ) );
		done_title->setFont( title_font );
		done_title->setAlignment( Qt::AlignHCenter );
		done_layout->addWidget( done_title );
		done_blurb_->setWordWrap( true );
		done_blurb_->setAlignment( Qt::AlignHCenter );
		done_blurb_->setTextFormat( Qt::RichText );
		done_layout->addWidget( done_blurb_ );
		done_layout->addStretch( 1 );
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

		parent->installEventFilter( this );
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
		done_blurb_->setText(
				QStringLiteral( "Hold <b>%1</b> and point at a word in any program.<br>"
		                        "Open <b>Help</b> in the sidebar any time for a short guide." )
						.arg( chordText( context_.settings->config().scan.trigger ).toHtmlEscaped() )
		);
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

			batch->ocr     = ocr_install::paddleFiles( selectedLanguages() );
			batch->ocr     = reinstall_ ? ocr_install::toDownload( std::move( batch->ocr ) ) : ocr_install::missingOnly( std::move( batch->ocr ) );
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
		if ( batch->ocr.empty() )
		{
			ocr_label_->setText( QStringLiteral( "OCR and runtime (already installed)" ) );
			ocr_bar_->setRange( 0, 1 );
			ocr_bar_->setValue( 1 );
		}
		else
		{
			ocr_label_->setText( QStringLiteral( "OCR and runtime" ) );
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

		install_status_->setText( QStringLiteral( "Downloading dictionaries, OCR models, and runtime together..." ) );

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

	void SetupWizard::finishInstall( const QString& error )
	{
		installing_           = false;
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
