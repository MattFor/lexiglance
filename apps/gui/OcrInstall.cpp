#include "OcrInstall.h"

#include "VcRedist.h"

#include <lexiglance/core/Paths.h>

#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QSysInfo>

#include <algorithm>

namespace lexiglance::gui::ocr_install
{

	namespace
	{

		constexpr auto onnx_runtime_version = "1.30.0";
		constexpr auto paddle_release       = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/";
		constexpr auto paddle_models        = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/";

#ifdef Q_OS_WIN
		constexpr auto runtime_library = "onnxruntime.dll";
		constexpr auto archive_suffix  = ".zip";
#elifdef Q_OS_MACOS
		constexpr auto runtime_library = "libonnxruntime.dylib";
		constexpr auto archive_suffix  = ".tgz";
		// Microsoft's last build for Intel Macs; Apple silicon gets onnx_runtime_version.
		constexpr auto onnx_runtime_intel_mac = "1.23.2";
#else
		constexpr auto runtime_library = "libonnxruntime.so";
		constexpr auto archive_suffix  = ".tgz";
#endif

	} // namespace

	QString ocrPath( const char* name )
	{
		return QString::fromStdString( ( paths::ocrDir() / name ).string() );
	}

	QString runtimeLibraryName()
	{
		return QString::fromUtf8( runtime_library );
	}

	std::vector<std::pair<QUrl, QString>> paddleFiles( std::span<const lang::Language* const> languages )
	{
		const QString                         models = ocrPath( "paddle" );
		std::vector<std::pair<QUrl, QString>> files{ { QUrl( QLatin1String( paddle_models ) + QStringLiteral( "det/PP-OCRv6_det_small.onnx" ) ), models + "/det.onnx" } };
		if ( std::ranges::any_of( languages, []( const lang::Language* language ) { return language->ocrModels().paddle.empty(); } ) )
		{
			files.emplace_back( QUrl( QLatin1String( paddle_models ) + QStringLiteral( "rec/PP-OCRv6_rec_small.onnx" ) ), models + "/rec.onnx" );
		}
		for ( const lang::Language* language : languages )
		{
			const auto    ocr    = language->ocrModels();
			const QString target = models + "/" + QString::fromUtf8( ocr.paddleFile().data(), static_cast<qsizetype>( ocr.paddleFile().size() ) );
			if ( !ocr.paddle.empty() && std::ranges::none_of( files, [&]( const auto& file ) { return file.second == target; } ) )
			{
				files.emplace_back( QUrl( QLatin1String( paddle_release ) + QString::fromUtf8( ocr.paddle.data(), static_cast<qsizetype>( ocr.paddle.size() ) ) ), target );
			}
		}
		return files;
	}

	bool paddleInstalled( std::span<const lang::Language* const> languages )
	{
		return std::ranges::all_of( paddleFiles( languages ), []( const auto& file ) { return QFile::exists( file.second ); } );
	}

	std::vector<std::pair<QUrl, QString>> toDownload( std::vector<std::pair<QUrl, QString>> files )
	{
		if ( !std::ranges::all_of( files, []( const auto& file ) { return QFile::exists( file.second ); } ) )
		{
			std::erase_if( files, []( const auto& file ) { return QFile::exists( file.second ); } );
		}
		return files;
	}

	std::vector<std::pair<QUrl, QString>> missingOnly( std::vector<std::pair<QUrl, QString>> files )
	{
		std::erase_if( files, []( const auto& file ) { return QFile::exists( file.second ); } );
		return files;
	}

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
		if ( QSysInfo::kernelType() == QStringLiteral( "darwin" ) )
		{
#ifdef Q_OS_MACOS
			if ( cpu == QStringLiteral( "arm64" ) )
			{
				return QStringLiteral( "onnxruntime-osx-arm64-" ) + onnx_runtime_version;
			}
			if ( cpu == QStringLiteral( "x86_64" ) )
			{
				return QStringLiteral( "onnxruntime-osx-x86_64-" ) + onnx_runtime_intel_mac;
			}
#endif
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

	QString unpackRuntime( const QString& directory, const QString& archive )
	{
		const QString packed = directory + "/" + archive + archive_suffix;
		if ( !QFile::exists( packed ) )
		{
			return QStringLiteral( "cannot unpack ONNX Runtime: the download is missing" );
		}
		// Archives are named after their version, which differs between builds (Intel Macs).
		const QString version = archive.section( u'-', -1 );
#ifdef Q_OS_WIN
		const QString     library = QStringLiteral( "lib/onnxruntime.dll" );
		const QString     program = qEnvironmentVariable( "SystemRoot", QStringLiteral( "C:\\Windows" ) ) + QStringLiteral( "\\System32\\tar.exe" );
		const QString     extract = QStringLiteral( "-xf" );
		const QStringList options{ QStringLiteral( "--strip-components=1" ) };
#elifdef Q_OS_MACOS
		const QString library = QStringLiteral( "lib/libonnxruntime.%1.dylib" ).arg( version );
		const QString program = QStringLiteral( "tar" );
		const QString extract = QStringLiteral( "-xzf" );
		// The Intel build's names start with ./, which --strip-components would count as the folder.
		const QStringList options{ QStringLiteral( "-s,^\\./,," ), QStringLiteral( "--strip-components=1" ) };
#else
		const QString     library = QStringLiteral( "lib/libonnxruntime.so." ) + version;
		const QString     program = QStringLiteral( "tar" );
		const QString     extract = QStringLiteral( "-xzf" );
		const QStringList options{ QStringLiteral( "--strip-components=1" ) };
#endif
		QDir().mkpath( directory );
		QProcess tar;
		tar.start( program, QStringList{ extract, packed, QStringLiteral( "-C" ), directory } + options + QStringList{ archive + "/LICENSE", archive + "/" + library } );
		const bool unpacked = tar.waitForFinished( 60000 ) && tar.exitStatus() == QProcess::NormalExit && tar.exitCode() == 0;
		QFile::remove( packed );
		if ( !unpacked )
		{
			return QStringLiteral( "cannot unpack ONNX Runtime: " ) + QString::fromLocal8Bit( tar.readAllStandardError() ).trimmed();
		}
		const QString extracted = directory + "/" + library;
		const QString installed = directory + "/" + runtime_library;
		if ( !QFile::exists( extracted ) )
		{
			return QStringLiteral( "cannot install ONNX Runtime: the library isn't in the download" );
		}
		if ( QFile::exists( installed ) && !QFile::remove( installed ) )
		{
			return QStringLiteral( "cannot install ONNX Runtime: Lexiglance is still using the old copy" );
		}
		if ( QFile::rename( extracted, installed ) )
		{
			QDir( directory + "/lib" ).removeRecursively();
			return {};
		}
		if ( QFile::copy( extracted, installed ) )
		{
			QFile::remove( extracted );
			QDir( directory + "/lib" ).removeRecursively();
			return {};
		}
		return QStringLiteral( "cannot install ONNX Runtime" );
	}

	QString askVcRedist( QWidget* parent )
	{
		const QString missing = vcredist::missing();
		if ( missing.isEmpty() )
		{
			return {};
		}
		const auto answer = QMessageBox::question(
				parent,
				QStringLiteral( "Microsoft Visual C++ Redistributable" ),
				QStringLiteral( "OCR needs the Microsoft Visual C++ Redistributable, which isn't installed on this computer (%1 can't be found). "
				                "Everything else in Lexiglance works without it.\n\nInstall it too? Windows will ask for permission." )
						.arg( missing )
		);
		return answer == QMessageBox::Yes ? vcredist::installerPath() : QString();
	}

	QString runtimePackedPath()
	{
		const QString archive = runtimeArchive();
		return archive.isEmpty() ? QString() : ocrPath( "runtime" ) + "/" + archive + archive_suffix;
	}

	void appendRuntime( std::vector<std::pair<QUrl, QString>>& files, QString* redist_out, QWidget* ask_parent, bool prompt_redist, bool force )
	{
		const QString runtime = ocrPath( "runtime" );
		const QString archive = runtimeArchive();
		const QString packed  = runtimePackedPath();
		if ( !packed.isEmpty() && ( force || !QFile::exists( runtime + "/" + runtime_library ) ) )
		{
			files.emplace_back( QUrl( QStringLiteral( "https://github.com/microsoft/onnxruntime/releases/download/v%1/%2%3" ).arg( archive.section( u'-', -1 ), archive, archive_suffix ) ), packed );
		}
		if ( redist_out != nullptr )
		{
			if ( prompt_redist )
			{
				*redist_out = askVcRedist( ask_parent );
			}
			else if ( !vcredist::missing().isEmpty() )
			{
				*redist_out = vcredist::installerPath();
			}
			else
			{
				*redist_out = {};
			}
			if ( !redist_out->isEmpty() )
			{
				files.emplace_back( vcredist::url(), *redist_out );
			}
		}
	}

} // namespace lexiglance::gui::ocr_install
