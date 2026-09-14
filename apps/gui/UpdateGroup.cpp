#include "UpdateGroup.h"

#include "Common.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Version.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QSysInfo>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <system_error>
#include <utility>

namespace lexiglance::gui
{

	enum class UpdateGroup::Kind
	{
		WindowsSetup,    // installed with lexiglance-windows-setup.exe
		WindowsPortable, // unpacked from lexiglance-windows-portable.zip
		AppImage,        // lexiglance-x86_64.AppImage
		Deb,             // the lexiglance-amd64.deb package
		Source,          // a build tree
		Other,           // the .tar.gz, or a system or processor there are no releases for
	};

	namespace
	{

		using Kind = UpdateGroup::Kind;

		Kind installKind()
		{
			const QDir here( QCoreApplication::applicationDirPath() );
			if ( here.exists( QStringLiteral( "../daemon" ) ) )
			{
				return Kind::Source;
			}
#ifdef Q_OS_WIN
			return here.exists( QStringLiteral( "../Uninstall.exe" ) ) ? Kind::WindowsSetup : Kind::WindowsPortable;
#else
			// Releases for Linux are built for x86-64 only.
			if ( QSysInfo::kernelType() != QStringLiteral( "linux" ) || QSysInfo::currentCpuArchitecture() != QStringLiteral( "x86_64" ) )
			{
				return Kind::Other;
			}
			if ( !qEnvironmentVariableIsEmpty( "APPIMAGE" ) )
			{
				return Kind::AppImage;
			}
			if ( here.absolutePath().startsWith( QStringLiteral( "/usr/" ) ) && QFileInfo::exists( QStringLiteral( "/var/lib/dpkg/info/lexiglance.list" ) ) )
			{
				return Kind::Deb;
			}
			return Kind::Other;
#endif
		}

		// The release file this copy updates from; empty when it cannot replace itself.
		QString assetName( Kind kind )
		{
			switch ( kind )
			{
				case Kind::WindowsSetup:
					return QStringLiteral( "lexiglance-windows-setup.exe" );
				case Kind::WindowsPortable:
					return QStringLiteral( "lexiglance-windows-portable.zip" );
				case Kind::AppImage:
					return QStringLiteral( "lexiglance-x86_64.AppImage" );
				case Kind::Deb:
					return QStringLiteral( "lexiglance-amd64.deb" );
				case Kind::Source:
				case Kind::Other:
					break;
			}
			return {};
		}

		QString describe( Kind kind )
		{
			switch ( kind )
			{
				case Kind::WindowsSetup:
					return QStringLiteral( "Installed with the Windows setup: an update installs the new setup in the background and opens Lexiglance again." );
				case Kind::WindowsPortable:
					return QStringLiteral( "The portable copy: an update unpacks the new .zip over this folder and opens Lexiglance again." );
				case Kind::AppImage:
					return QStringLiteral( "The AppImage: an update replaces %1 and opens Lexiglance again." ).arg( qEnvironmentVariable( "APPIMAGE" ) );
				case Kind::Deb:
					return QStringLiteral( "Installed from the .deb package: an update installs the new package, which asks for your password." );
				case Kind::Source:
					return QStringLiteral( "Built from source: update it with git pull and a rebuild." );
				case Kind::Other:
					break;
			}
			return QStringLiteral( "This copy cannot replace itself: Download gets the newest build for this system." );
		}

		// Installing without asking: not the .deb, whose password prompt would come out of nowhere.
		bool installsAutomatically( Kind kind )
		{
			return kind == Kind::WindowsSetup || kind == Kind::WindowsPortable || kind == Kind::AppImage;
		}

		// The newest build for this system, for copies that cannot replace themselves.
		QUrl downloadPage()
		{
			const QString latest = qs( project_url ) + QStringLiteral( "/releases/latest" );
			if ( QSysInfo::kernelType() == QStringLiteral( "linux" ) && QSysInfo::currentCpuArchitecture() == QStringLiteral( "x86_64" ) )
			{
				return QUrl( latest + QStringLiteral( "/download/lexiglance-x86_64.AppImage" ) );
			}
			return QUrl( latest );
		}

		QString updateDirectory()
		{
			return qs( ( paths::cacheDir() / "update" ).string() );
		}

		// The checksum SHA256SUMS ("<sha256>  <name>" lines) lists for `name`.
		QByteArray listedChecksum( const QByteArray& sums, const QString& name )
		{
			for ( const QByteArray& line : sums.split( '\n' ) )
			{
				const QList<QByteArray> fields = line.simplified().split( ' ' );
				if ( fields.size() == 2 && ( fields[1] == name.toUtf8() || fields[1] == "*" + name.toUtf8() ) )
				{
					return fields[0].toLower();
				}
			}
			return {};
		}

		QByteArray checksum( const QString& path )
		{
			QFile              file( path );
			QCryptographicHash hash( QCryptographicHash::Sha256 );
			return file.open( QIODevice::ReadOnly ) && hash.addData( &file ) ? hash.result().toHex() : QByteArray();
		}

		// The portable update, run by Windows' PowerShell once this program has ended: this copy's programs end, the new
		// .zip replaces its bin, lib and share (the old ones come back if that fails), and it starts again.
		constexpr const char* portable_script = R"ps(param([string]$Zip, [string]$Target, [int]$Wait, [string]$Arguments)
$ErrorActionPreference = 'Stop'
$log = Join-Path ([IO.Path]::GetTempPath()) 'lexiglance-update.log'
$prefix = $Target.TrimEnd('\') + '\'
try {
    Wait-Process -Id $Wait -Timeout 30 -ErrorAction SilentlyContinue
    $unpacked = Join-Path ([IO.Path]::GetTempPath()) 'lexiglance-update'
    if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
    Expand-Archive -LiteralPath $Zip -DestinationPath $unpacked -Force
    $from = if (Test-Path (Join-Path $unpacked 'bin')) { $unpacked } else { (Get-ChildItem $unpacked -Directory | Select-Object -First 1).FullName }
    Get-Process lexiglance, lexiglanced, lexiglancectl -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    $parts = 'bin', 'lib', 'share'
    foreach ($part in $parts) { $old = Join-Path $Target $part; if (Test-Path $old) { Rename-Item $old "$part.old" } }
    try {
        Copy-Item -Path (Join-Path $from '*') -Destination $Target -Recurse -Force
    } catch {
        foreach ($part in $parts) {
            $new = Join-Path $Target $part
            if (Test-Path "$new.old") { if (Test-Path $new) { Remove-Item $new -Recurse -Force }; Rename-Item "$new.old" $part }
        }
        throw
    }
    foreach ($part in $parts) { $old = Join-Path $Target "$part.old"; if (Test-Path $old) { Remove-Item $old -Recurse -Force } }
    Remove-Item $unpacked -Recurse -Force -ErrorAction SilentlyContinue
} catch {
    Add-Content -Path $log -Value "$(Get-Date): $_"
}
Start-Process -FilePath (Join-Path $Target 'bin\lexiglance.exe') -ArgumentList $Arguments
)ps";

		void repolish( QWidget* widget )
		{
			widget->style()->unpolish( widget );
			widget->style()->polish( widget );
		}

	} // namespace

	UpdateGroup::UpdateGroup( QWidget* parent ) :
		QGroupBox( QStringLiteral( "Updates" ), parent ),
		downloader_( new Downloader( this ) ),
		status_( new QLabel() ),
		note_( new QLabel() ),
		update_( new QPushButton() ),
		automatic_( new QCheckBox() ),
		progress_( new QProgressBar() )
	{
		const Kind kind   = installKind();
		auto*      layout = new QVBoxLayout( this );
		auto*      row    = new QHBoxLayout();
		status_->setWordWrap( true );
		status_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		row->addWidget( status_, 1 );
		row->addWidget( update_ );
		layout->addLayout( row );
		progress_->hide();
		layout->addWidget( progress_ );
		automatic_->setText( installsAutomatically( kind ) ? QStringLiteral( "Install updates automatically" ) : QStringLiteral( "Check for updates automatically" ) );
		automatic_->setToolTip( QStringLiteral( "Looks for a new version when Lexiglance starts and every six hours." ) );
		layout->addWidget( automatic_ );
		note_->setText( describe( kind ) );
		note_->setWordWrap( true );
		note_->setEnabled( false );
		layout->addWidget( note_ );

		auto memory = applicationMemory();
		automatic_->setChecked( memory.value( QStringLiteral( "update/automatic" ), true ).toBool() );
		status_->setText( QStringLiteral( "This is Lexiglance %1." ).arg( qs( version ) ) );
		// Started again by an update: it took.
		if ( memory.value( QStringLiteral( "update/installing" ) ).toString() == qs( version ) )
		{
			memory.remove( QStringLiteral( "update/installing" ) );
			QDir( updateDirectory() ).removeRecursively();
			status_->setText( QStringLiteral( "Updated to Lexiglance %1." ).arg( qs( version ) ) );
		}
		showState();

		connect( update_, &QPushButton::clicked, this, [this] { update( false ); } );
		connect( automatic_, &QCheckBox::toggled, this, [this]( bool on ) {
			applicationMemory().setValue( QStringLiteral( "update/automatic" ), on );
			runAutomatically();
		} );
		auto* timer = new QTimer( this );
		timer->setInterval( 6 * 60 * 60 * 1000 );
		connect( timer, &QTimer::timeout, this, [this] { runAutomatically(); } );
		timer->start();
		// Not while everything else starts.
		QTimer::singleShot( 20000, this, [this] { runAutomatically(); } );
	}

	void UpdateGroup::runAutomatically()
	{
		if ( !automatic_->isChecked() || busy_ )
		{
			return;
		}
		if ( installsAutomatically( installKind() ) )
		{
			update( true );
		}
		else
		{
			check();
		}
	}

	void UpdateGroup::check( std::function<void()> then )
	{
		status_->setText( QStringLiteral( "Looking for a newer version..." ) );
		downloader_->resolve( QUrl( qs( project_url ) + QStringLiteral( "/releases/latest" ) ), [this, then = std::move( then )]( const QUrl& url, const QString& error ) {
			// It leads to the newest release's page, .../releases/tag/v1.0.2.
			const QString path = url.path();
			if ( !error.isEmpty() || !path.contains( QStringLiteral( "/releases/tag/" ) ) )
			{
				fail( QStringLiteral( "Cannot find the newest version: %1" ).arg( error.isEmpty() ? QStringLiteral( "no release is published" ) : error ) );
				return;
			}
			const QString tag = path.section( '/', -1 );
			latest_           = tag.startsWith( 'v' ) ? tag.mid( 1 ) : tag;
			checked_          = QTime::currentTime().toString( QStringLiteral( "HH:mm" ) );
			showState();
			if ( then )
			{
				then();
			}
		} );
	}

	void UpdateGroup::update( bool automatic )
	{
		const Kind kind = installKind();
		if ( kind == Kind::Other )
		{
			QDesktopServices::openUrl( downloadPage() );
			return;
		}
		if ( assetName( kind ).isEmpty() || busy_ )
		{
			return;
		}
		setBusy( true );
		check( [this, automatic, kind] {
			if ( automatic )
			{
				// An update that did not take (this is still the older version) is not tried on its own again for a day.
				const auto memory       = applicationMemory();
				const bool tried_before = memory.value( QStringLiteral( "update/installing" ) ).toString() == latest_ && QDateTime::currentSecsSinceEpoch() - memory.value( QStringLiteral( "update/started" ) ).toLongLong() < 24LL * 60 * 60;
				if ( !olderVersion( qs( version ), latest_ ) || tried_before )
				{
					setBusy( false );
					return;
				}
			}
			download( kind );
		} );
	}

	void UpdateGroup::download( Kind kind )
	{
		const QString name = assetName( kind );
		const QString base = qs( project_url ) + QStringLiteral( "/releases/download/v%1/" ).arg( latest_ );
		// Next to the AppImage, to replace it in one step; elsewhere with the other downloads.
		const QString target = kind == Kind::AppImage ? qEnvironmentVariable( "APPIMAGE" ) + QStringLiteral( ".update" ) : updateDirectory() + "/" + name;
		status_->setText( QStringLiteral( "Downloading %1..." ).arg( name ) );
		downloader_->fetch( QUrl( base + QStringLiteral( "SHA256SUMS" ) ), [this, kind, name, base, target]( const QByteArray& sums, const QString& error ) {
			const QByteArray expected = listedChecksum( sums, name );
			if ( !error.isEmpty() || expected.isEmpty() )
			{
				fail( QStringLiteral( "Update failed: %1" ).arg( error.isEmpty() ? QStringLiteral( "the release lists no checksum for %1" ).arg( name ) : error ) );
				return;
			}
			downloader_->download(
					QUrl( base + name ),
					target,
					[this]( qint64 received, qint64 total ) { showProgress( progress_, received, total ); },
					[this, kind, target, expected]( const QString& download_error ) {
						if ( !download_error.isEmpty() )
						{
							fail( QStringLiteral( "Update failed: %1" ).arg( download_error ) );
							return;
						}
						if ( checksum( target ) != expected )
						{
							QFile::remove( target );
							fail( QStringLiteral( "Update failed: the download was damaged (its checksum does not match). Try again." ) );
							return;
						}
						install( kind, target );
					}
			);
		} );
	}

	void UpdateGroup::install( Kind kind, const QString& file )
	{
		// The new version starts as this one is: hidden in the tray, or showing these updates.
		const bool  tray      = !window()->isVisible();
		QStringList arguments = tray ? QStringList{ QStringLiteral( "--tray" ) } : QStringList{ QStringLiteral( "--page" ), QStringLiteral( "overview" ) };
		arguments << QStringLiteral( "--updated" );
		{
			auto memory = applicationMemory();
			memory.setValue( QStringLiteral( "update/installing" ), latest_ );
			memory.setValue( QStringLiteral( "update/started" ), QDateTime::currentSecsSinceEpoch() );
		}
		status_->setText( QStringLiteral( "Installing Lexiglance %1..." ).arg( latest_ ) );
		progress_->hide();

		switch ( kind )
		{
			case Kind::WindowsSetup:
				// The setup ends this program and its daemon, installs without a window and opens the new version.
				if ( !QProcess::startDetached( file, { QStringLiteral( "/S" ), tray ? QStringLiteral( "/relaunch=tray" ) : QStringLiteral( "/relaunch=window" ) } ) )
				{
					fail( QStringLiteral( "Update failed: cannot start %1" ).arg( file ) );
					return;
				}
				QApplication::quit();
				return;

			case Kind::WindowsPortable:
			{
				const QString script = updateDirectory() + QStringLiteral( "/update-portable.ps1" );
				QFile         out( script );
				if ( !out.open( QIODevice::WriteOnly | QIODevice::Truncate ) || out.write( portable_script ) < 0 )
				{
					fail( QStringLiteral( "Update failed: cannot write %1" ).arg( script ) );
					return;
				}
				out.close();
				const QString powershell = qEnvironmentVariable( "SystemRoot", QStringLiteral( "C:\\Windows" ) ) + QStringLiteral( "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" );
				const QString root       = QDir( QCoreApplication::applicationDirPath() + QStringLiteral( "/.." ) ).absolutePath();
				if ( !QProcess::startDetached( powershell, { QStringLiteral( "-NoProfile" ), QStringLiteral( "-ExecutionPolicy" ), QStringLiteral( "Bypass" ), QStringLiteral( "-WindowStyle" ), QStringLiteral( "Hidden" ), QStringLiteral( "-File" ), QDir::toNativeSeparators( script ), QStringLiteral( "-Zip" ), QDir::toNativeSeparators( file ), QStringLiteral( "-Target" ), QDir::toNativeSeparators( root ), QStringLiteral( "-Wait" ), QString::number( QCoreApplication::applicationPid() ), QStringLiteral( "-Arguments" ), arguments.join( ' ' ) } ) )
				{
					fail( QStringLiteral( "Update failed: cannot start PowerShell" ) );
					return;
				}
				QApplication::quit();
				return;
			}

			case Kind::AppImage:
			{
				const QString appimage = qEnvironmentVariable( "APPIMAGE" );
				QFile::setPermissions( file, QFile::permissions( appimage ) | QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner );
				// One step: the running AppImage keeps its old file until it ends.
				std::error_code ec;
				std::filesystem::rename( std::filesystem::path( ss( file ) ), std::filesystem::path( ss( appimage ) ), ec );
				if ( ec )
				{
					QFile::remove( file );
					fail( QStringLiteral( "Update failed: cannot replace %1 (%2)" ).arg( appimage, qs( ec.message() ) ) );
					return;
				}
				QProcess::startDetached( appimage, arguments );
				QApplication::quit();
				return;
			}

			case Kind::Deb:
			{
				const QString pkexec = QStandardPaths::findExecutable( QStringLiteral( "pkexec" ) );
				if ( pkexec.isEmpty() )
				{
					fail( QStringLiteral( "Update failed: installing needs pkexec. Install %1 with apt instead." ).arg( file ) );
					return;
				}
				auto* apt = new QProcess( this );
				connect( apt, &QProcess::finished, this, [this, apt, arguments]( int code, QProcess::ExitStatus status ) {
					apt->deleteLater();
					if ( status != QProcess::NormalExit || code != 0 )
					{
						// pkexec: 126 when the password dialog was dismissed, 127 when it was wrong.
						const QString output = QString::fromLocal8Bit( apt->readAllStandardError() ).trimmed().section( '\n', -1 );
						fail( code == 126 || code == 127 ? QStringLiteral( "Update cancelled: the password was not given." ) : QStringLiteral( "Update failed: apt-get: %1" ).arg( output ) );
						return;
					}
					QProcess::startDetached( QCoreApplication::applicationFilePath(), arguments );
					QApplication::quit();
				} );
				apt->start( pkexec, { QStringLiteral( "apt-get" ), QStringLiteral( "install" ), QStringLiteral( "--reinstall" ), QStringLiteral( "--allow-downgrades" ), QStringLiteral( "-y" ), file } );
				return;
			}

			case Kind::Source:
			case Kind::Other:
				setBusy( false );
				return;
		}
	}

	void UpdateGroup::showState()
	{
		const Kind    kind    = installKind();
		const QString current = qs( version );
		const bool    newer   = !latest_.isEmpty() && olderVersion( current, latest_ );
		const bool    ahead   = !latest_.isEmpty() && olderVersion( latest_, current );
		if ( !latest_.isEmpty() )
		{
			if ( newer )
			{
				status_->setText( QStringLiteral( "Lexiglance %1 is out (this is %2)." ).arg( latest_, current ) );
			}
			else if ( ahead )
			{
				status_->setText( QStringLiteral( "This is Lexiglance %1, newer than the latest release (%2)." ).arg( current, latest_ ) );
			}
			else
			{
				status_->setText( QStringLiteral( "Lexiglance %1 is the newest version. Checked at %2." ).arg( current, checked_ ) );
			}
		}

		update_->setVisible( kind != Kind::Source );
		if ( kind == Kind::Other )
		{
			update_->setText( QStringLiteral( "Download the latest" ) );
		}
		else if ( latest_.isEmpty() )
		{
			update_->setText( QStringLiteral( "Update to the latest" ) );
		}
		else
		{
			update_->setText( ( newer ? QStringLiteral( "Update to %1" ) : ahead ? QStringLiteral( "Install %1" )
			                                                                     : QStringLiteral( "Reinstall %1" ) )
			                          .arg( latest_ ) );
		}
		update_->setToolTip( kind == Kind::Other ? QStringLiteral( "Opens the download of the newest release for this system" ) : QStringLiteral( "Downloads %1 from the newest release and installs it, even when it is this version" ).arg( assetName( kind ) ) );
		update_->setProperty( "primary", newer );
		repolish( update_ );
	}

	void UpdateGroup::fail( const QString& message )
	{
		status_->setText( message );
		progress_->hide();
		setBusy( false );
	}

	void UpdateGroup::setBusy( bool busy )
	{
		busy_ = busy;
		update_->setEnabled( !busy );
	}

} // namespace lexiglance::gui
