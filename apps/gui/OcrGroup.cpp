#include "OcrGroup.h"

#include "DaemonClient.h"
#include "Settings.h"
#include "VcRedist.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/language/Language.h>

#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProcess>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	namespace
	{

		const QString onnx_runtime_version = QStringLiteral( "1.30.0" );
		// RapidOCR's ONNX exports of the PaddleOCR models; languages name theirs relative to it (lang::OcrModels).
		const QString paddle_release = QStringLiteral( "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/" );
		const QString paddle_models  = paddle_release + QStringLiteral( "PP-OCRv6/" );

		QString ocrPath( const char* name )
		{
			return qs( ( paths::ocrDir() / name ).string() );
		}

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

		// PaddleOCR's files for the languages, each with where it comes from: the detector, the default recogniser
		// (Chinese, Japanese, English) when a language needs it, and the recognisers of the others.
		std::vector<std::pair<QUrl, QString>> paddleFiles( std::span<const lang::Language* const> languages )
		{
			const QString                         models = ocrPath( "paddle" );
			std::vector<std::pair<QUrl, QString>> files{ { QUrl( paddle_models + "det/PP-OCRv6_det_small.onnx" ), models + "/det.onnx" } };
			if ( std::ranges::any_of( languages, []( const lang::Language* language ) { return language->ocrModels().paddle.empty(); } ) )
			{
				files.emplace_back( QUrl( paddle_models + "rec/PP-OCRv6_rec_small.onnx" ), models + "/rec.onnx" );
			}
			for ( const lang::Language* language : languages )
			{
				const auto    ocr    = language->ocrModels();
				const QString target = models + "/" + qs( ocr.paddleFile() );
				if ( !ocr.paddle.empty() && std::ranges::none_of( files, [&]( const auto& file ) { return file.second == target; } ) )
				{
					files.emplace_back( QUrl( paddle_release + qs( ocr.paddle ) ), target );
				}
			}
			return files;
		}

		bool paddleInstalled( std::span<const lang::Language* const> languages )
		{
			return std::ranges::all_of( paddleFiles( languages ), []( const auto& file ) { return QFile::exists( file.second ); } );
		}

		// What is missing, or everything again when nothing is.
		std::vector<std::pair<QUrl, QString>> toDownload( std::vector<std::pair<QUrl, QString>> files )
		{
			if ( !std::ranges::all_of( files, []( const auto& file ) { return QFile::exists( file.second ); } ) )
			{
				std::erase_if( files, []( const auto& file ) { return QFile::exists( file.second ); } );
			}
			return files;
		}

#ifdef Q_OS_WIN
		// The runtime's library as the daemon loads it, and the release archives' format.
		const QString runtime_library = QStringLiteral( "onnxruntime.dll" );
		const QString archive_suffix  = QStringLiteral( ".zip" );
#else
		const QString runtime_library = QStringLiteral( "libonnxruntime.so" );
		const QString archive_suffix  = QStringLiteral( ".tgz" );
#endif

		// The ONNX Runtime release archive for this machine (without extension), if one exists.
		QString runtimeArchive()
		{
			const QString cpu = QSysInfo::currentCpuArchitecture();
			if ( QSysInfo::kernelType() == QStringLiteral( "winnt" ) )
			{
				if ( cpu == QStringLiteral( "x86_64" ) )
				{
					return QStringLiteral( "onnxruntime-win-x64-" ) + onnx_runtime_version;
				}
				if ( cpu == QStringLiteral( "arm64" ) )
				{
					return QStringLiteral( "onnxruntime-win-arm64-" ) + onnx_runtime_version;
				}
				return {};
			}
			if ( QSysInfo::kernelType() != QStringLiteral( "linux" ) )
			{
				return {};
			}
			if ( cpu == QStringLiteral( "x86_64" ) )
			{
				return QStringLiteral( "onnxruntime-linux-x64-" ) + onnx_runtime_version;
			}
			if ( cpu == QStringLiteral( "arm64" ) )
			{
				return QStringLiteral( "onnxruntime-linux-aarch64-" ) + onnx_runtime_version;
			}
			return {};
		}

		// Takes the library and its license out of a downloaded release archive; an error message on failure.
		QString unpackRuntime( const QString& directory, const QString& archive )
		{
			const QString packed = directory + "/" + archive + archive_suffix;
#ifdef Q_OS_WIN
			const QString library = QStringLiteral( "lib/onnxruntime.dll" );
			// Windows' own bsdtar (not another tar on PATH), which reads zip archives too.
			const QString program = qEnvironmentVariable( "SystemRoot", QStringLiteral( "C:\\Windows" ) ) + QStringLiteral( "\\System32\\tar.exe" );
			const QString extract = QStringLiteral( "-xf" );
#else
			const QString library = "lib/libonnxruntime.so." + onnx_runtime_version;
			const QString program = QStringLiteral( "tar" );
			const QString extract = QStringLiteral( "-xzf" );
#endif
			QProcess tar;
			tar.start( program, { extract, packed, QStringLiteral( "-C" ), directory, QStringLiteral( "--strip-components=1" ), archive + "/LICENSE", archive + "/" + library } );
			const bool unpacked = tar.waitForFinished( 60000 ) && tar.exitStatus() == QProcess::NormalExit && tar.exitCode() == 0;
			QFile::remove( packed );
			if ( !unpacked )
			{
				return QStringLiteral( "cannot unpack ONNX Runtime: " ) + QString::fromLocal8Bit( tar.readAllStandardError() ).trimmed();
			}
			QFile::remove( directory + "/" + runtime_library );
			const bool moved = QFile::rename( directory + "/" + library, directory + "/" + runtime_library );
			QDir( directory + "/lib" ).removeRecursively();
			return moved ? QString() : QStringLiteral( "cannot install ONNX Runtime" );
		}

		// Whether to fetch the Visual C++ redistributable along with PaddleOCR: where to put it, or empty when this
		// machine already has it or the user would rather not. Downloading the runtime is the moment it starts to
		// matter, and for a portable copy or a build from source it is the only moment there is.
		QString askVcRedist( QWidget* parent )
		{
			// Empty off Windows, so nothing below happens there.
			const QString missing = vcredist::missing();
			if ( missing.isEmpty() )
			{
				return {};
			}
			const auto answer = QMessageBox::question(
					parent,
					QStringLiteral( "Microsoft Visual C++ Redistributable" ),
					QStringLiteral( "Reading text from the screen needs the Microsoft Visual C++ Redistributable, which this computer does not have "
			                        "(%1). Everything else in Lexiglance works without it.\n\nInstall it as well? Windows will ask for permission." )
							.arg( missing )
			);
			return answer == QMessageBox::Yes ? vcredist::installerPath() : QString();
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
			if ( paddleInstalled( languages ) )
			{
				text = QStringLiteral( "Re-download PaddleOCR" );
			}
			else if ( QFile::exists( ocrPath( "paddle" ) + "/det.onnx" ) )
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
		fetchAll( QStringLiteral( "Tesseract models" ), toDownload( std::move( files ) ), {} );
	}

	void OcrGroup::downloadPaddle()
	{
		const QString runtime = ocrPath( "runtime" );
		auto          files   = toDownload( paddleFiles( ocrLanguages() ) );
		// ONNX Runtime comes along unless it is already there; elsewhere it has to be installed from the system.
		const QString archive = runtimeArchive();
		const bool    unpack  = !archive.isEmpty() && !QFile::exists( runtime + "/" + runtime_library );
		if ( unpack )
		{
			files.emplace_back( QUrl( QStringLiteral( "https://github.com/microsoft/onnxruntime/releases/download/v%1/%2%3" ).arg( onnx_runtime_version, archive, archive_suffix ) ), runtime + "/" + archive + archive_suffix );
		}
		// That runtime is Microsoft's own build and imports their redistributable, which Lexiglance itself does not.
		// It is installed once everything is downloaded (installRedist), not here.
		redist_ = askVcRedist( this );
		if ( !redist_.isEmpty() )
		{
			files.emplace_back( vcredist::url(), redist_ );
		}
		fetchAll( QStringLiteral( "PaddleOCR" ), std::move( files ), [runtime, archive, unpack] { return unpack ? unpackRuntime( runtime, archive ) : QString(); } );
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
