#include "SetupWizard.h"

#include "DaemonClient.h"
#include "OcrInstall.h"
#include "Settings.h"
#include "TranslationInstall.h"
#include "VcRedist.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStyle>
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

		// Every download that failed, grouped by what went wrong, so being offline reads as one line, not ten.
		QString failureText( const std::vector<std::pair<QString, QString>>& failures )
		{
			QStringList                 errors;
			QHash<QString, QStringList> names;
			for ( const auto& [name, error] : failures )
			{
				if ( !names.contains( error ) )
				{
					errors << error;
				}
				if ( !names[error].contains( name ) )
				{
					names[error] << name;
				}
			}
			QString text;
			for ( const QString& error : errors )
			{
				text += QStringLiteral( "\n• %1: %2" ).arg( names[error].join( QStringLiteral( ", " ) ), error );
			}
			return text;
		}

		// What a downloaded file is for, in the list of failed downloads.
		QString fileLabel( const QString& target, const QString& redist )
		{
			if ( target == ocr_install::runtimePackedPath() )
			{
				return QStringLiteral( "ONNX Runtime" );
			}
			if ( target == redist )
			{
				return QStringLiteral( "Visual C++ Redistributable" );
			}
			return QDir::fromNativeSeparators( target ).contains( QStringLiteral( "/translation/" ) ) ? QStringLiteral( "translation models" ) : QStringLiteral( "OCR models" );
		}

		void setPrimary( QPushButton* button, bool primary )
		{
			button->setProperty( "primary", primary );
			button->style()->unpolish( button );
			button->style()->polish( button );
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
		std::int64_t                                            import_job          = -1;
		QString                                                 import_name;
		std::vector<std::pair<QString, QString>>                failures; // what, why
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
		continue_( new QPushButton( QStringLiteral( "Continue" ) ) ),
		skip_( new QPushButton( QStringLiteral( "Skip for now" ) ) ),
		install_status_( new QLabel() ),
		overall_label_( new QLabel( QStringLiteral( "Overall" ) ) ),
		overall_bar_( new QProgressBar() ),
		dict_label_( new QLabel( QStringLiteral( "Dictionaries" ) ) ),
		dict_bar_( new QProgressBar() ),
		ocr_label_( new QLabel( QStringLiteral( "OCR and runtime" ) ) ),
		ocr_bar_( new QProgressBar() ),
		ocr_box_( new QCheckBox( QStringLiteral( "Read text in games and videos too (OCR)" ) ) ),
		ocr_note_( new QLabel( QStringLiteral( "For games, videos and images, where a program doesn't share its text. If you skip it now, it's on the Scanning page later." ) ) ),
		translation_box_( new QCheckBox( QStringLiteral( "Translate sentences into English too" ) ) ),
		translation_note_( new QLabel() ),
		tips_( new QLabel() ),
		continue_anyway_( new QPushButton( QStringLiteral( "Continue while it downloads" ) ) ),
		done_blurb_( new QLabel() ),
		permissions_( new QFrame() ),
		permission_rows_( new QVBoxLayout() ),
		practice_( new QTextBrowser() ),
		practice_status_( new QLabel() ),
		downloads_note_( new QLabel() ),
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

		auto* blurb = new QLabel( QStringLiteral( "A pop-up dictionary for the languages you're learning, in any program." ) );
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
		features->addWidget( feature( QStringLiteral( "edit-find" ), QStringLiteral( "Point at a word" ), QStringLiteral( "Hold a key over text anywhere: a browser, a chat, a game, a video. A popup shows what it means." ) ) );
		features->addWidget( feature( QStringLiteral( "preferences-desktop-locale" ), QStringLiteral( "Whole sentences in English" ), QStringLiteral( "Hold one more key and the sentence is translated, right on this computer." ) ) );
		features->addWidget( feature( QStringLiteral( "accessories-dictionary" ), QStringLiteral( "Dictionaries ready to use" ), QStringLiteral( "For %1. Setup downloads them for you in the next step." ).arg( listed( offered ) ) ) );
		features->addWidget( feature( QStringLiteral( "document-send" ), QStringLiteral( "Keep what you learn" ), QStringLiteral( "Send words to Anki along with the sentence they came from." ) ) );
		welcome_layout->addLayout( features );

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
		auto* languages_blurb = new QLabel( QStringLiteral( "Setup downloads the recommended dictionaries for each language you tick. You can change your "
		                                                    "languages later on the Scanning page." ) );
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
		ocr_box_->setChecked( true );
		languages_layout->addWidget( ocr_box_ );
		ocr_note_->setWordWrap( true );
		ocr_note_->setEnabled( false );
		ocr_note_->setContentsMargins( 26, 0, 0, 6 );
		languages_layout->addWidget( ocr_note_ );
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
		install_status_->setText( QStringLiteral( "Getting ready..." ) );
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

		// There from the start: offline, every download waits for the connection to time out.
		continue_anyway_->setToolTip( QStringLiteral( "The downloads keep going while you try Lexiglance out." ) );
		auto* install_footer = new QHBoxLayout();
		install_footer->addStretch( 1 );
		install_footer->addWidget( continue_anyway_ );
		install_layout->addStretch( 1 );
		install_layout->addLayout( install_footer );
		pages_->addWidget( install_page );
		connect( continue_anyway_, &QPushButton::clicked, this, [this] { showPage( Page::Done ); } );

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
		permissions_->setObjectName( QStringLiteral( "healthRow" ) );
		auto* permissions_layout = new QVBoxLayout( permissions_ );
		auto* permissions_text   = new QLabel( QStringLiteral( "<b>First, allow Lexiglance in System Settings.</b> macOS needs your permission before Lexiglance can see "
		                                                       "the trigger and read the text under the pointer. Turn Lexiglance on in each list, then reopen it if macOS asks." ) );
		permissions_text->setWordWrap( true );
		permissions_layout->addWidget( permissions_text );
		permissions_layout->addLayout( permission_rows_ );
		permissions_->hide();
		done_layout->addSpacing( 6 );
		done_layout->addWidget( permissions_ );
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
		downloads_note_->setWordWrap( true );
		downloads_note_->setAlignment( Qt::AlignHCenter );
		downloads_note_->hide();
		done_layout->addSpacing( 6 );
		done_layout->addWidget( downloads_note_ );
		done_layout->addStretch( 1 );
		auto* later = new QLabel( QStringLiteral( "Everything else is in Help, at the bottom of the sidebar." ) );
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
			// A dictionary is in place once the daemon has imported it, not when it queued the job.
			if ( name == "import.finished" && import_batch_ && params["job"].asInt() == import_batch_->import_job )
			{
				importDone( params["ok"].asBool() ? QString() : qs( params["error"].asString() ) );
				return;
			}
			if ( !isVisible() || pages_->currentIndex() != static_cast<int>( Page::Done ) )
			{
				return;
			}
			if ( name == "trigger.changed" && params["held"].asBool() && !tried_ )
			{
				practice_status_->setText( QStringLiteral( "Now point at a word in the sentence." ) );
			}
			else if ( name == "capture.result" && params["found"].asBool() && params["entries"].asInt() > 0 )
			{
				tried_ = true;
				practice_status_->setText( QStringLiteral( "<span style=\"color:#3fa45b; font-weight:600\">✓ It works.</span> Do the same in any program." ) );
			}
			else if ( name == "capture.result" && !tried_ )
			{
				practice_status_->setText( dicts_pending_ ? QStringLiteral( "<span style=\"color:#d19a1f\">Nothing found yet</span>: the dictionary is still downloading." ) : QStringLiteral( "<span style=\"color:#d19a1f\">Nothing found there.</span> Point at the middle of a word." ) );
			}
		} );
		context_.client->onConnection( [this]( bool connected ) {
			if ( !connected && import_batch_ )
			{
				importDone( QStringLiteral( "Lexiglance stopped during the import" ) );
			}
		} );

#ifdef Q_OS_MACOS
		// Back from System Settings.
		connect( qApp, &QGuiApplication::applicationStateChanged, this, [this]( Qt::ApplicationState state ) {
			if ( state == Qt::ApplicationActive && isVisible() && pages_->currentIndex() == static_cast<int>( Page::Done ) )
			{
				refreshPermissions();
			}
		} );
#endif

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
		skip_->setVisible( !required_ );
		const auto& cfg       = context_.settings->config();
		const bool  first_run = !applicationMemory().value( QStringLiteral( "setup/completed" ) ).toBool();
		// Languages already chosen once (a restart during the downloads): as they were chosen.
		const bool chosen = applicationMemory().value( QStringLiteral( "setup/chosen" ) ).toBool();
		for ( std::size_t i = 0; i < language_boxes_.size(); ++i )
		{
			if ( first_run && !chosen )
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
		// Back after a restart (macOS's Quit & Reopen for a permission, or during the downloads): where it was left.
		Page resume = Page::Welcome;
		if ( first_run && applicationMemory().value( QStringLiteral( "setup/installed" ) ).toBool() )
		{
			resume = Page::Done;
		}
		else if ( first_run && chosen )
		{
			resume = Page::Languages;
		}
		showPage( resume );
		continue_->setText( QStringLiteral( "Continue" ) );
		continue_->setEnabled( !locked() );
		skip_->setEnabled( !locked() );
		if ( locked() )
		{
			// The seconds left on the button, which says why it can't be pressed yet.
			auto*      countdown = new QTimer( this );
			const auto tick      = [this, countdown] {
				if ( const qint64 left = ( unlock_at_ms_ - QDateTime::currentMSecsSinceEpoch() + 999 ) / 1000; left > 0 )
				{
					continue_->setText( QStringLiteral( "Continue (%1)" ).arg( left ) );
					return;
				}
				countdown->deleteLater();
				continue_->setText( QStringLiteral( "Continue" ) );
				continue_->setEnabled( true );
				skip_->setEnabled( true );
				if ( QPushButton* button = primaryButton() )
				{
					button->setFocus();
				}
			};
			connect( countdown, &QTimer::timeout, this, tick );
			countdown->start( 250 );
			tick();
		}
		place();
		show();
		raise();
		if ( QPushButton* button = primaryButton(); button != nullptr && !locked() )
		{
			button->setFocus();
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
		continue_anyway_->setText( QStringLiteral( "Continue while it downloads" ) );
		setPrimary( continue_anyway_, false );
		continue_anyway_->show();
		downloads_note_->hide();
		tips_->show();
		dict_label_->setText( QStringLiteral( "Dictionaries" ) );
		dict_label_->setEnabled( false );
		dict_bar_->setRange( 0, 1 );
		dict_bar_->setValue( 0 );
		dict_bar_->setFormat( QStringLiteral( "%p%" ) );
		ocr_label_->setText( QStringLiteral( "OCR and translation models" ) );
		ocr_label_->setEnabled( false );
		ocr_bar_->setRange( 0, 1 );
		ocr_bar_->setValue( 0 );
		ocr_bar_->setFormat( QStringLiteral( "%p%" ) );
		install_status_->setText( QStringLiteral( "Getting ready..." ) );
	}

	void SetupWizard::setOverall( int value, int maximum, const QString& text )
	{
		overall_label_->setText( text );
		overall_label_->setEnabled( true );
		overall_bar_->setRange( 0, std::max( 1, maximum ) );
		overall_bar_->setValue( std::clamp( value, 0, overall_bar_->maximum() ) );
		overall_bar_->setFormat( QStringLiteral( "%v / %m" ) );
		if ( installing_ )
		{
			downloads_note_->setText( QStringLiteral( "Still downloading in the background: %1 of %2 files done." ).arg( overall_bar_->value() ).arg( overall_bar_->maximum() ) );
			if ( downloads_note_->isHidden() )
			{
				downloads_note_->show();
				place();
			}
		}
	}

	QPushButton* SetupWizard::primaryButton() const
	{
		for ( auto* button : pages_->currentWidget()->findChildren<QPushButton*>() )
		{
			if ( button->property( "primary" ).toBool() && button->isEnabled() && button->isVisibleTo( pages_->currentWidget() ) )
			{
				return button;
			}
		}
		return nullptr;
	}

	void SetupWizard::place()
	{
		setGeometry( parentWidget()->rect() );
		card_->ensurePolished();
		// Its cached height knows nothing of what was shown or hidden on a page since.
		card_->layout()->invalidate();
		const int width = std::min( 580, this->width() - 48 );
		// As tall as the tallest page needs: at least the usual 560, at most the window.
		const int room   = this->height() - 48;
		const int height = std::clamp( card_->heightForWidth( width ), std::min( 560, room ), room );
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
		// Buttons outside a dialog take only Space: Enter presses the one in focus, or the page's main one.
		if ( event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter )
		{
			auto* focused = qobject_cast<QPushButton*>( focusWidget() );
			if ( QPushButton* button = focused != nullptr ? focused : primaryButton() )
			{
				button->click();
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
		return !required_ && !locked();
	}

	void SetupWizard::showPage( Page page )
	{
		pages_->setCurrentIndex( static_cast<int>( page ) );
		place();
		if ( QPushButton* button = primaryButton(); button != nullptr && isVisible() )
		{
			button->setFocus();
		}
		if ( page == Page::Done )
		{
			applicationMemory().setValue( QStringLiteral( "setup/installed" ), true );
			updatePractice();
#ifdef Q_OS_MACOS
			refreshPermissions();
#endif
		}
	}

	void SetupWizard::refreshPermissions()
	{
		context_.client->call(
				"health",
				"{}",
				[this]( const json::Value* report, const QString& ) {
					while ( QLayoutItem* item = permission_rows_->takeAt( 0 ) )
					{
						delete item->widget();
						delete item;
					}
					// A row for each list in System Settings, which several checks can point at.
					QStringList listed;
					for ( const health::Check& check : report != nullptr ? health::read( *report ) : std::vector<health::Check>() )
					{
						const QString action = qs( check.fix );
						if ( check.status == health::Severity::Ok || !action.startsWith( QStringLiteral( "open-privacy-" ) ) || listed.contains( action ) )
						{
							continue;
						}
						listed << action;
						auto* row    = new QWidget();
						auto* layout = new QHBoxLayout( row );
						layout->setContentsMargins( 0, 0, 0, 0 );
						// Named as the list is in System Settings, where it is looked for.
						static const QHash<QString, QString> lists{
							{ QStringLiteral( "open-privacy-accessibility" ), QStringLiteral( "Accessibility" ) },
							{ QStringLiteral( "open-privacy-input-monitoring" ), QStringLiteral( "Input Monitoring" ) },
							{ QStringLiteral( "open-privacy-screen-recording" ), QStringLiteral( "Screen & System Audio Recording" ) },
						};
						auto* title = new QLabel( lists.value( action, qs( check.title ) ) );
						title->setToolTip( qs( check.detail ) );
						auto* open = new QPushButton( QStringLiteral( "Open System Settings" ) );
						connect( open, &QPushButton::clicked, this, [action] { openPrivacySettings( action ); } );
						layout->addWidget( title, 1 );
						layout->addWidget( open );
						permission_rows_->addWidget( row );
					}
					permissions_->setVisible( permission_rows_->count() > 0 );
					place();
				},
				30000
		);
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
		if ( ocr_box_->isChecked() && config.scan.ocr == config::OcrMode::Off )
		{
			config.scan.ocr = config::OcrMode::Fallback;
		}
		context_.settings->commit();
		applicationMemory().setValue( QStringLiteral( "setup/chosen" ), true );
	}

	void SetupWizard::startInstall()
	{
		installing_ = true;
		showPage( Page::Install );
		resetProgress();

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

			batch->ocr = ocr_box_->isChecked() ? ocr_install::paddleFiles( selectedLanguages() ) : std::vector<std::pair<QUrl, QString>>();
			batch->ocr = reinstall_ ? ocr_install::toDownload( std::move( batch->ocr ) ) : ocr_install::missingOnly( std::move( batch->ocr ) );
			// The translation models come with them, over the same runtime.
			if ( translation_box_->isChecked() )
			{
				std::ranges::move( translation_install::files( translation_install::wanted( selectedLanguages(), context_.settings->config().translation ), reinstall_ ), std::back_inserter( batch->ocr ) );
			}
			batch->runtime = ocr_install::ocrPath( "runtime" );
			batch->archive = ocr_install::runtimeArchive();
			redist_.clear();
			// ONNX Runtime runs OCR and translation alike.
			if ( ocr_box_->isChecked() || !batch->ocr.empty() )
			{
				ocr_install::appendRuntime( batch->ocr, &redist_, this, false, reinstall_ );
			}
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
		const bool    ocr         = ocr_box_->isChecked();
		const bool    translation = translation_box_->isChecked() && translation_box_->isEnabled();
		const QString models      = ocr ? ( translation ? QStringLiteral( "OCR and translation models" ) : QStringLiteral( "OCR models" ) ) : QStringLiteral( "Translation models" );
		const bool    any_models  = ocr || translation || !batch->ocr.empty();
		ocr_label_->setVisible( any_models );
		ocr_bar_->setVisible( any_models );
		dicts_pending_ = !batch->dicts.empty();
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

		const auto files = batch->dicts.size() + batch->ocr.size();
		install_status_->setText( QStringLiteral( "Downloading %1 %2..." ).arg( files ).arg( files == 1 ? QStringLiteral( "file" ) : QStringLiteral( "files" ) ) );

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
					[this, batch, name = fileLabel( batch->ocr[i].second, redist_ )]( const QString& error ) { onOcrDownloadDone( batch, name, error ); }
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
		if ( !error.isEmpty() )
		{
			batch->failures.emplace_back( name, error );
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
		dicts_pending_ = batch->dict_imports_left > 0;
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
		batch->import_name = name;
		install_status_->setText( QStringLiteral( "Installing %1..." ).arg( name ) );
		dict_label_->setText( QStringLiteral( "Dictionaries: installing %1" ).arg( name ) );
		json::Writer params;
		params.beginObject().field( "path", ss( target ) ).field( "delete_source", true ).field( "replace", true ).endObject();
		context_.client->call( "dictionaries.import", params.take(), [this, batch]( const json::Value* result, const QString& import_error ) {
			import_batch_ = batch;
			if ( result == nullptr )
			{
				importDone( import_error );
				return;
			}
			batch->import_job = ( *result )["job"].asInt();
		} );
	}

	void SetupWizard::importDone( const QString& error )
	{
		const auto batch = std::exchange( import_batch_, {} );
		if ( !error.isEmpty() )
		{
			batch->failures.emplace_back( batch->import_name, error );
		}
		batch->importing = false;
		if ( batch->dict_imports_left > 0 )
		{
			--batch->dict_imports_left;
		}
		dicts_pending_ = batch->dict_imports_left > 0;
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
	}

	void SetupWizard::onOcrDownloadDone( const std::shared_ptr<InstallBatch>& batch, const QString& name, const QString& error )
	{
		if ( !error.isEmpty() )
		{
			batch->failures.emplace_back( name, error );
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
			}
			setOverall( overall_bar_->maximum(), overall_bar_->maximum(), QStringLiteral( "Overall" ) );

			if ( !redist_.isEmpty() )
			{
				const QString installer = std::exchange( redist_, QString() );
				install_status_->setText( QStringLiteral( "Installing the Microsoft Visual C++ Redistributable, which ONNX Runtime needs..." ) );
				vcredist::install( this, installer, [this]( const QString& redist_error ) {
					if ( !redist_error.isEmpty() )
					{
						finishInstall( failureText( { { QStringLiteral( "Visual C++ Redistributable" ), redist_error } } ) );
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

		if ( batch->failures.empty() && batch->unpack_runtime )
		{
			install_status_->setText( QStringLiteral( "Unpacking ONNX Runtime, which runs OCR and translation..." ) );
			const QString installed = batch->runtime + "/" + ocr_install::runtimeLibraryName();
			const auto    unpack    = [this, batch, after_runtime, installed] {
				QString error = ocr_install::unpackRuntime( batch->runtime, batch->archive );
				// Simulate / reinstall: if the daemon still held the library, keep the working copy already on disk.
				if ( !error.isEmpty() && reinstall_ && QFile::exists( installed ) )
				{
					log::warn( "setup: {} (keeping the ONNX Runtime already installed)", ss( error ) );
					error.clear();
				}
				after_runtime( error.isEmpty() ? QString() : failureText( { { QStringLiteral( "ONNX Runtime" ), error } } ) );
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

		if ( !batch->failures.empty() )
		{
			after_runtime( failureText( batch->failures ) );
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
		translation_note_->setText( sentence.isEmpty() ? QStringLiteral( "It's translated on this computer, so nothing you read is sent anywhere." ) : QStringLiteral( "Hold %1 with the trigger to see the sentence in English. It's translated on this computer, so nothing you read is sent anywhere." ).arg( sentence ) );
		QString    tips = QStringLiteral( "<b>While you wait, here's how it works</b><table cellspacing=\"0\" cellpadding=\"3\" style=\"margin-top:6px\">" );
		const auto tip  = [&]( const QString& what, const QString& how ) { tips += QStringLiteral( "<tr><td style=\"padding-right:14px\">%1</td><td>%2</td></tr>" ).arg( what, how ); };
		tip( QStringLiteral( "Look up a word" ), QStringLiteral( "Hold <b>%1</b> and point at it" ).arg( trigger ) );
		tip( QStringLiteral( "Select more characters" ), QStringLiteral( "Keep holding it and turn the scroll wheel" ) );
		if ( !sentence.isEmpty() )
		{
			tip( QStringLiteral( "Translate the sentence" ), QStringLiteral( "Hold <b>%1</b> as well" ).arg( sentence ) );
		}
		tip( QStringLiteral( "Use the popup" ), QStringLiteral( "Move onto it before you let go. Scroll for more entries, click one to copy it" ) );
		tips_->setText( tips + QStringLiteral( "</table>" ) );
		done_blurb_->setText( sentence.isEmpty() ? QStringLiteral( "Hold <b>%1</b> and point at a word below." ).arg( trigger ) : QStringLiteral( "Hold <b>%1</b> and point at a word below. Add <b>%2</b> to see the sentence in English." ).arg( trigger, sentence ) );
	}

	void SetupWizard::updatePractice()
	{
		const auto chosen = selectedLanguages();
		tried_            = false;
		practice_status_->setText( QStringLiteral( "<span style=\"color:gray\">Tip: keep holding the keys and turn the scroll wheel to select more characters.</span>" ) );
		QString html;
		if ( !chosen.empty() )
		{
			// The sentence, and words every general dictionary has.
			QStringList words;
			for ( const std::string_view word : chosen.front()->sampleWords() )
			{
				words << qs( word ).toHtmlEscaped();
			}
			html = QStringLiteral( "<p align=\"center\">%1</p><p align=\"center\">%2</p>" ).arg( qs( chosen.front()->exampleSentence() ).toHtmlEscaped(), words.join( QStringLiteral( "&nbsp;&nbsp;&nbsp;·&nbsp;&nbsp;&nbsp;" ) ) );
		}
		practice_->setHtml( html );
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
		installing_    = false;
		dicts_pending_ = false;
		// Translation models that came are loaded when next needed.
		context_.client->call( "translation.reload" );
		if ( !error.isEmpty() )
		{
			log::warn( "setup: {}", ss( QString( error ).replace( QStringLiteral( "\n• " ), QStringLiteral( "; " ) ).mid( 2 ) ) );
			install_status_->setText( QStringLiteral( "Some downloads didn't work:%1\n\nGet them later on the Dictionaries, Scanning (OCR) and Translation pages." ).arg( error ) );
			downloads_note_->setText( QStringLiteral( "Some downloads didn't work. Get them later on the Dictionaries, Scanning (OCR) and Translation pages." ) );
			downloads_note_->show();
			// Only what went wrong and the way on: the tips and the bars would push it out of the card, and a full bar reads as done.
			tips_->hide();
			for ( QWidget* widget : std::initializer_list<QWidget*>{ overall_label_, overall_bar_, dict_label_, dict_bar_, ocr_label_, ocr_bar_ } )
			{
				widget->hide();
			}
			continue_anyway_->setText( QStringLiteral( "Continue anyway" ) );
			setPrimary( continue_anyway_, true );
			if ( pages_->currentIndex() == static_cast<int>( Page::Install ) )
			{
				continue_anyway_->setFocus();
			}
			place();
			return;
		}
		continue_anyway_->hide();
		// Already trying it (Continue while it downloads): only say so.
		if ( pages_->currentIndex() == static_cast<int>( Page::Done ) )
		{
			downloads_note_->setText( QStringLiteral( "All downloads finished." ) );
			return;
		}
		downloads_note_->hide();
		if ( isVisible() )
		{
			showPage( Page::Done );
		}
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
