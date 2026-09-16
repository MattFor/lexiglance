#include "VcRedist.h"

#include "Common.h"

#include <lexiglance/ocr/Onnx.h>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSysInfo>

namespace lexiglance::gui::vcredist
{

	QString missing()
	{
		return qs( ocr::missingVcRuntime() );
	}

	QUrl url()
	{
		const QString architecture = QSysInfo::currentCpuArchitecture() == QStringLiteral( "arm64" ) ? QStringLiteral( "arm64" ) : QStringLiteral( "x64" );
		return { QStringLiteral( "https://aka.ms/vs/17/release/vc_redist.%1.exe" ).arg( architecture ) };
	}

	QString installerPath()
	{
		return QDir::tempPath() + QStringLiteral( "/lexiglance-vc-redist.exe" );
	}

	void install( QObject* owner, const QString& installer, const std::function<void( const QString& )>& done )
	{
		auto* process = new QProcess( owner );
		QObject::connect( process, &QProcess::finished, owner, [process, installer, done]( int code, QProcess::ExitStatus status ) {
			process->deleteLater();
			QFile::remove( installer );
			// 1638: a newer one is already installed. 3010: installed, and a restart would finish the job.
			if ( status == QProcess::NormalExit && ( code == 0 || code == 1638 || code == 3010 ) )
			{
				done( {} );
				return;
			}
			// 1602 and 1223: the permission prompt was dismissed, which is an answer rather than a fault.
			if ( code == 1602 || code == 1223 )
			{
				done( QStringLiteral( "the Visual C++ Redistributable was not installed, so OCR cannot read anything yet" ) );
				return;
			}
			done( QStringLiteral( "the Visual C++ Redistributable installer failed (code %1); it can be installed by hand from %2" ).arg( code ).arg( url().toString() ) );
		} );
		// A process that never starts sends no finished(), so its answer comes from here instead.
		QObject::connect( process, &QProcess::errorOccurred, owner, [process, installer, done]( QProcess::ProcessError error ) {
			if ( error != QProcess::FailedToStart )
			{
				return;
			}
			process->deleteLater();
			QFile::remove( installer );
			done( QStringLiteral( "the Visual C++ Redistributable installer could not be started (%1)" ).arg( installer ) );
		} );
		process->start( installer, { QStringLiteral( "/install" ), QStringLiteral( "/passive" ), QStringLiteral( "/norestart" ) } );
	}

} // namespace lexiglance::gui::vcredist
