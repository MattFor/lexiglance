#include "OcrGroup.h"

#include "DaemonClient.h"
#include "OcrInstall.h"
#include "Settings.h"
#include "VcRedist.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/language/Language.h>

#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		QString modelDirectory( const std::string& model )
		{
			return qs( ( paths::ocrDir() / model ).string() );
		}

		// Tesseract's models of the languages, horizontal and vertical.
		std::vector<QString> tesseractModels( std::span<const lang::Language* const> languages )
		{
			std::vector<QString> names;
			for ( const lang::Language* language : languages )
			{
				for ( const std::string_view name : { language->ocrModels().tesseract, language->ocrModels().tesseract_vertical } )
				{
					if ( !name.empty() )
					{
						names.push_back( qs( name ) );
					}
				}
			}
			return names;
		}

		bool modelInstalled( const std::string& model, std::span<const lang::Language* const> languages )
		{
			const QString directory = modelDirectory( model );
			return std::ranges::all_of( tesseractModels( languages ), [&]( const QString& name ) { return QFile::exists( directory + "/" + name + ".traineddata" ); } );
		}

		int modeIndex( config::OcrMode mode )
		{
			switch ( mode )
			{
				case config::OcrMode::Off:
					return 0;
				case config::OcrMode::Always:
					return 2;
				case config::OcrMode::Fallback:
					break;
			}
			return 1;
		}

		config::OcrMode modeAt( int index )
		{
			switch ( index )
			{
				case 0:
					return config::OcrMode::Off;
				case 2:
					return config::OcrMode::Always;
				default:
					return config::OcrMode::Fallback;
			}
		}

		int engineIndex( config::OcrEngine engine )
		{
			switch ( engine )
			{
				case config::OcrEngine::Paddle:
					return 1;
				case config::OcrEngine::Tesseract:
					return 2;
				case config::OcrEngine::Auto:
					break;
			}
			return 0;
		}

		config::OcrEngine engineAt( int index )
		{
			switch ( index )
			{
				case 1:
					return config::OcrEngine::Paddle;
				case 2:
					return config::OcrEngine::Tesseract;
				default:
					return config::OcrEngine::Auto;
			}
		}

		QLabel* note( const QString& text )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setEnabled( false );
			return label;
		}

	} // namespace

	OcrGroup::OcrGroup( Context context, QWidget* parent ) :
		QGroupBox( QStringLiteral( "Screen text recognition (OCR)" ), parent ),
		context_( std::move( context ) ),
		downloader_( new Downloader( this ) ),
		mode_( new QComboBox() ),
		engine_( new QComboBox() ),
		vertical_( new QCheckBox( QStringLiteral( "Vertical text (manga, vertical novels)" ) ) ),
		model_( new QComboBox() ),
		windows_( new QPlainTextEdit() ),
		status_( new QLabel() ),
		download_( new QPushButton( QIcon::fromTheme( QStringLiteral( "folder-download" ) ), QString() ) ),
		runtime_( new QPushButton( QIcon::fromTheme( QStringLiteral( "folder-download" ) ), QStringLiteral( "Install the Visual C++ Redistributable" ) ) ),
		progress_( new QProgressBar() )
	{
		auto* layout = new QVBoxLayout( this );
		layout->addWidget( note( QStringLiteral( "Reads the pixels around the pointer when an application exposes no text: games, images, videos, "
		                                         "Wine programs and some terminals. Only a region around the pointer is read, and only while the trigger is held." ) ) );

		auto* form = new QFormLayout();
		mode_->addItems( { QStringLiteral( "Off" ), QStringLiteral( "When an application exposes no text" ), QStringLiteral( "Always" ) } );
		engine_->addItems( { QStringLiteral( "Automatic (PaddleOCR when installed)" ), QStringLiteral( "PaddleOCR (most accurate)" ), QStringLiteral( "Tesseract" ) } );
		model_->addItems( { QStringLiteral( "Fast" ), QStringLiteral( "Accurate (slower)" ) } );
		form->addRow( QStringLiteral( "Use OCR" ), mode_ );
		form->addRow( QStringLiteral( "Engine" ), engine_ );
		form->addRow( QStringLiteral( "Tesseract model" ), model_ );
		form->addRow( vertical_ );
		layout->addLayout( form );

		layout->addWidget( note( QStringLiteral( "Windows that always use OCR first (window class patterns, one per line):" ) ) );
		windows_->setMaximumHeight( 80 );
		windows_->setPlaceholderText( QStringLiteral( "steam_app_*" ) );
		layout->addWidget( windows_ );

		auto* row = new QHBoxLayout();
		status_->setWordWrap( true );
		status_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		row->addWidget( status_, 1 );
		// Only on a Windows that has no Visual C++ Redistributable, which PaddleOCR's runtime imports (updateDownloadButton).
		runtime_->setToolTip( QStringLiteral( "Microsoft's runtime, which their build of ONNX Runtime needs and PaddleOCR cannot read anything without. Windows will ask for permission, and Lexiglance starts again on it." ) );
		runtime_->hide();
		row->addWidget( runtime_ );
		row->addWidget( download_ );
		layout->addLayout( row );
		progress_->hide();
		layout->addWidget( progress_ );

		connect( mode_, &QComboBox::currentIndexChanged, this, [this] { store(); } );
		for ( QComboBox* combo : { engine_, model_ } )
		{
			connect( combo, &QComboBox::currentIndexChanged, this, [this] {
				store();
				updateStatus();
			} );
		}
		connect( vertical_, &QCheckBox::toggled, this, [this] { store(); } );
		connect( windows_, &QPlainTextEdit::textChanged, this, [this] { store(); } );
		connect( download_, &QPushButton::clicked, this, [this] {
			if ( engine_->currentIndex() == 2 )
			{
				downloadTesseract();
			}
			else
			{
				downloadPaddle();
			}
		} );
		connect( runtime_, &QPushButton::clicked, this, [this] { downloadRedist(); } );

		context_.client->onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( name == "config.changed" )
			{
				updateStatus();
			}
			// Sent once the daemon has rebuilt its text capture, after a download say.
			else if ( name == "status.changed" )
			{
				showStatus( params );
			}
		} );
	}

	void OcrGroup::refresh()
	{
		loading_         = true;
		const auto& scan = context_.settings->config().scan;
		{
			const QSignalBlocker a( mode_ );
			const QSignalBlocker b( model_ );
			const QSignalBlocker c( vertical_ );
			const QSignalBlocker d( engine_ );
			mode_->setCurrentIndex( modeIndex( scan.ocr ) );
			engine_->setCurrentIndex( engineIndex( scan.ocr_engine ) );
			model_->setCurrentIndex( scan.ocr_model == "best" ? 1 : 0 );
			vertical_->setChecked( scan.ocr_vertical );
		}
		QStringList patterns;
		for ( const auto& pattern : scan.ocr_windows )
		{
			patterns << qs( pattern );
		}
		if ( windows_->toPlainText() != patterns.join( '\n' ) )
		{
			const QSignalBlocker blocker( windows_ );
			windows_->setPlainText( patterns.join( '\n' ) );
		}
		loading_ = false;
		updateStatus();
	}

	void OcrGroup::store()
	{
		if ( loading_ )
		{
			return;
		}
		auto& scan        = context_.settings->config().scan;
		scan.ocr          = modeAt( mode_->currentIndex() );
		scan.ocr_engine   = engineAt( engine_->currentIndex() );
		scan.ocr_model    = model_->currentIndex() == 1 ? "best" : "fast";
		scan.ocr_vertical = vertical_->isChecked();
		scan.ocr_windows.clear();
		for ( const QString& line : windows_->toPlainText().split( '\n', Qt::SkipEmptyParts ) )
		{
			if ( !line.trimmed().isEmpty() )
			{
				scan.ocr_windows.push_back( ss( line.trimmed() ) );
			}
		}
		context_.settings->commit();
	}

	void OcrGroup::updateStatus()
	{
		updateDownloadButton();
		context_.client->call( "status", "{}", [this]( const json::Value* status, const QString& error ) {
			if ( status == nullptr )
			{
				status_->setText( error );
				return;
			}
			showStatus( *status );
		} );
	}

	void OcrGroup::showStatus( const json::Value& status )
	{
		// The reason in full here, where the download that fixes it is: the overview keeps to the summary.
		const QString problem = qs( status["capture_problem"].asString() );
		status_->setText( QStringLiteral( "Text capture: " ) + qs( status["capture"].asString() ) + ( problem.isEmpty() ? QString() : "\n" + problem ) );
		// Models are for the languages the daemon reads: turned on, with dictionaries.
		std::vector<std::string> in_use;
		for ( const json::Value& code : status["languages"].items() )
		{
			in_use.emplace_back( code.asString() );
		}
		if ( in_use != in_use_ )
		{
			in_use_ = std::move( in_use );
			updateDownloadButton();
		}
	}

	void OcrGroup::updateDownloadButton()
	{
		const bool tesseract = engine_->currentIndex() == 2;
		const auto languages = ocrLanguages();
		model_->setEnabled( engine_->currentIndex() != 1 );
		// Nothing PaddleOCR downloads runs without Microsoft's runtime, and Tesseract needs none of it.
		runtime_->setVisible( !tesseract && !vcredist::missing().isEmpty() );
		if ( tesseract )
		{
			download_->setText( modelInstalled( model_->currentIndex() == 1 ? "best" : "fast", languages ) ? QStringLiteral( "Re-download models" ) : QStringLiteral( "Download Tesseract models" ) );
		}
		else
		{
			QString text = QStringLiteral( "Download PaddleOCR (about 40 MB)" );
			if ( ocr_install::paddleInstalled( languages ) )
			{
				text = QStringLiteral( "Re-download PaddleOCR" );
			}
			else if ( QFile::exists( ocr_install::ocrPath( "paddle" ) + "/det.onnx" ) )
			{
				text = QStringLiteral( "Download PaddleOCR for more languages" );
			}
			download_->setText( text );
		}
	}

	std::vector<const lang::Language*> OcrGroup::ocrLanguages() const
	{
		const auto                         enabled = lang::enabledLanguages( context_.settings->config().disabled_languages );
		std::vector<const lang::Language*> used;
		std::ranges::copy_if( enabled, std::back_inserter( used ), [this]( const lang::Language* language ) { return std::ranges::contains( in_use_, language->code() ); } );
		return used.empty() ? enabled : used;
	}

	void OcrGroup::fetchAll( const QString& what, std::vector<std::pair<QUrl, QString>> files, std::function<QString()> finish )
	{
		download_->setEnabled( false );
		runtime_->setEnabled( false );
		status_->setText( QStringLiteral( "Downloading %1..." ).arg( what ) );
		auto remaining = std::make_shared<std::size_t>( files.size() );
		auto failure   = std::make_shared<QString>();
		auto after     = std::make_shared<std::function<QString()>>( std::move( finish ) );
		// Each file's bytes so far and in all: the files come side by side, and the bar shows them as one.
		auto sizes = std::make_shared<std::vector<std::pair<qint64, qint64>>>( files.size() );
		for ( std::size_t index = 0; index < files.size(); ++index )
		{
			downloader_->download(
					files[index].first,
					files[index].second,
					[this, sizes, index]( qint64 received, qint64 total ) {
						( *sizes )[index] = { received, total };
						qint64 done       = 0;
						qint64 all        = 0;
						bool   known      = true;
						for ( const auto& [file_received, file_total] : *sizes )
						{
							done += file_received;
							all += file_total;
							known = known && file_total > 0;
						}
						showProgress( progress_, done, known ? all : 0 );
					},
					[this, remaining, failure, after]( const QString& error ) {
						if ( !error.isEmpty() )
						{
							*failure = error;
						}
						if ( --*remaining > 0 )
						{
							return;
						}
						if ( failure->isEmpty() && *after )
						{
							*failure = ( *after )();
						}
						download_->setEnabled( true );
						runtime_->setEnabled( true );
						progress_->hide();
						if ( !failure->isEmpty() )
						{
							// Whatever was downloaded towards the runtime is not installed after a failure.
							QFile::remove( std::exchange( redist_, QString() ) );
							status_->setText( QStringLiteral( "Download failed: " ) + *failure );
							return;
						}
						updateDownloadButton();
						// Windows' own runtime, which came along with the files: installed, and then the daemon is
				        // started again, since it can only find the new libraries as it starts.
						if ( !redist_.isEmpty() )
						{
							installRedist();
							return;
						}
						// The daemon rebuilds its text capture with the new files, and says so when it is ready (status.changed).
						status_->setText( QStringLiteral( "Text capture: loading what was downloaded..." ) );
						context_.client->call( "capture.reset", "{}", [this]( const json::Value*, const QString& reset_error ) {
							if ( !reset_error.isEmpty() )
							{
								updateStatus();
							}
						} );
					}
			);
		}
	}

	void OcrGroup::downloadTesseract()
	{
		const std::string                     model     = model_->currentIndex() == 1 ? "best" : "fast";
		const QString                         base      = model == "best" ? QStringLiteral( "https://github.com/tesseract-ocr/tessdata_best/raw/main/" ) : QStringLiteral( "https://github.com/tesseract-ocr/tessdata_fast/raw/main/" );
		const QString                         directory = modelDirectory( model );
		std::vector<std::pair<QUrl, QString>> files;
		for ( const QString& name : tesseractModels( ocrLanguages() ) )
		{
			files.emplace_back( QUrl( base + name + ".traineddata" ), directory + "/" + name + ".traineddata" );
		}
		fetchAll( QStringLiteral( "Tesseract models" ), ocr_install::toDownload( std::move( files ) ), {} );
	}

	void OcrGroup::downloadPaddle()
	{
		const QString runtime = ocr_install::ocrPath( "runtime" );
		auto          files   = ocr_install::toDownload( ocr_install::paddleFiles( ocrLanguages() ) );
		const QString archive = ocr_install::runtimeArchive();
		const bool    unpack  = !archive.isEmpty() && !QFile::exists( runtime + "/" + ocr_install::runtimeLibraryName() );
		ocr_install::appendRuntime( files, &redist_, this );
		fetchAll( QStringLiteral( "PaddleOCR" ), std::move( files ), [runtime, archive, unpack] {
			return unpack ? ocr_install::unpackRuntime( runtime, archive ) : QString();
		} );
	}

	void OcrGroup::downloadRedist()
	{
		redist_ = vcredist::installerPath();
		fetchAll( QStringLiteral( "the Visual C++ Redistributable" ), { { vcredist::url(), redist_ } }, {} );
	}

	void OcrGroup::installRedist()
	{
		const QString installer = std::exchange( redist_, QString() );
		status_->setText( QStringLiteral( "Installing the Microsoft Visual C++ Redistributable; Windows asks for permission." ) );
		download_->setEnabled( false );
		runtime_->setEnabled( false );
		log::info( "installing the Visual C++ Redistributable ({})", ss( installer ) );
		vcredist::install( this, installer, [this]( const QString& error ) {
			download_->setEnabled( true );
			runtime_->setEnabled( true );
			updateDownloadButton();
			if ( !error.isEmpty() )
			{
				log::error( "the Visual C++ Redistributable was not installed: {}", ss( error ) );
				status_->setText( QStringLiteral( "Download failed: " ) + error );
				return;
			}
			log::info( "the Visual C++ Redistributable is installed; starting the daemon again" );
			status_->setText( QStringLiteral( "Text capture: starting Lexiglance again on the new runtime..." ) );
			context_.client->startDaemon( true );
		} );
	}

} // namespace lexiglance::gui
