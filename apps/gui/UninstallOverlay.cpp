#include "UninstallOverlay.h"

#include "Common.h"
#include "DaemonClient.h"

#include <lexiglance/core/Log.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <iterator>

#ifdef Q_OS_WIN
	#include <QDir>
	#include <QFile>
	#include <QFileInfo>
	#include <QSettings>
#endif

namespace lexiglance::gui
{

	namespace
	{

		namespace fs = std::filesystem;

		QLabel* wrapped( const QString& text = {} )
		{
			auto* label = new QLabel( text );
			label->setWordWrap( true );
			label->setTextInteractionFlags( Qt::TextSelectableByMouse );
			return label;
		}

		QString bulleted( const std::vector<std::string>& lines )
		{
			QString html = QStringLiteral( "<ul style=\"margin-left: -24px\">" );
			for ( const std::string& line : lines )
			{
				html += QStringLiteral( "<li>%1</li>" ).arg( qs( line ).toHtmlEscaped() );
			}
			return html + QStringLiteral( "</ul>" );
		}

#ifdef Q_OS_WIN
		// A PowerShell string that holds `text` as it is.
		QString literal( const QString& text )
		{
			return "'" + QString( text ).replace( '\'', QStringLiteral( "''" ) ) + "'";
		}

		// Waits for the settings application to end, then deletes the copy's folders and, unless kept, the settings,
		// dictionaries and models; then the folders that are left empty, and itself.
		constexpr const char* removal_script = R"ps(param([int]$Wait)
$ErrorActionPreference = 'SilentlyContinue'
Wait-Process -Id $Wait -Timeout 30
Get-Process lexiglance, lexiglanced, lexiglancectl | Where-Object { $_.Path -and $_.Path.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force
Start-Sleep -Milliseconds 500
foreach ($path in $remove) { Remove-Item -LiteralPath $path -Recurse -Force }
foreach ($folder in $emptied) { if ((Test-Path -LiteralPath $folder) -and -not (Get-ChildItem -LiteralPath $folder -Force)) { Remove-Item -LiteralPath $folder -Force } }
Remove-Item -LiteralPath $PSCommandPath -Force
)ps";
#endif

	} // namespace

	UninstallOverlay::UninstallOverlay( DaemonClient* client, QWidget* parent ) :
		CardOverlay( 620, parent ),
		client_( client ),
		text_( wrapped() ),
		erase_( new QCheckBox( QStringLiteral( "Also delete my settings, dictionaries and downloaded models" ) ) ),
		erase_note_( wrapped( QStringLiteral( "Untick to keep them for a later install." ) ) ),
		status_( wrapped() ),
		cancel_( new QPushButton( QStringLiteral( "Cancel" ) ) ),
		uninstall_( new QPushButton( QStringLiteral( "Uninstall" ) ) )
	{
		auto* layout = new QVBoxLayout( card() );
		layout->setContentsMargins( 26, 20, 26, 18 );
		layout->setSpacing( 12 );
		auto* title      = new QLabel( QStringLiteral( "Uninstall Lexiglance" ) );
		QFont title_font = title->font();
		title_font.setPointSizeF( title_font.pointSizeF() * 1.4 );
		title_font.setBold( true );
		title->setFont( title_font );
		layout->addWidget( title );
		text_->setTextFormat( Qt::RichText );
		layout->addWidget( text_ );
		layout->addWidget( erase_ );
		erase_note_->setEnabled( false );
		erase_note_->setContentsMargins( 24, 0, 0, 0 );
		layout->addWidget( erase_note_ );
		status_->hide();
		layout->addWidget( status_ );

		auto* footer = new QHBoxLayout();
		footer->addStretch( 1 );
		footer->addWidget( cancel_ );
		uninstall_->setProperty( "primary", true );
		footer->addWidget( uninstall_ );
		layout->addLayout( footer );

		connect( erase_, &QCheckBox::toggled, this, [this] {
			describe();
			place();
		} );
		connect( cancel_, &QPushButton::clicked, this, [this] { hide(); } );
		connect( uninstall_, &QPushButton::clicked, this, [this] {
			if ( finished_ )
			{
				QApplication::quit();
				return;
			}
			start();
		} );
	}

	void UninstallOverlay::open()
	{
		if ( !finished_ )
		{
			erase_->setChecked( true );
			status_->hide();
			setBusy( false );
			describe();
		}
		popUp();
		cancel_->setFocus();
	}

	void UninstallOverlay::describe()
	{
		const auto plan = uninstall::plan( uninstall::Places::current(), !erase_->isChecked() );
		text_->setText( QStringLiteral( "This removes:" ) + bulleted( uninstall::describe( plan ) ) );
	}

	void UninstallOverlay::setBusy( bool busy )
	{
		setDismissible( !busy );
		erase_->setEnabled( !busy );
		cancel_->setEnabled( !busy );
		uninstall_->setEnabled( !busy );
	}

	void UninstallOverlay::fail( const QString& message )
	{
		log::warn( "uninstall: {}", ss( message ) );
		status_->setText( message );
		status_->show();
		setBusy( false );
		place();
	}

	void UninstallOverlay::start()
	{
		const bool keep = !erase_->isChecked();
		const auto plan = uninstall::plan( uninstall::Places::current(), keep );
		log::info( "uninstall: {} (keeping the data: {})", uninstall::describe( plan ).front(), keep );
		setBusy( true );
		status_->setText( QStringLiteral( "Uninstalling..." ) );
		status_->show();
		place();

		if ( plan.kind == uninstall::Kind::WindowsSetup )
		{
			// It ends this program and the daemon, and removes everything else.
			QStringList options{ QStringLiteral( "/S" ) };
			if ( keep )
			{
				options << QStringLiteral( "/KEEPDATA" );
			}
			if ( !QProcess::startDetached( qs( plan.where.string() ), options ) )
			{
				fail( QStringLiteral( "Cannot start %1. Nothing was removed." ).arg( qs( plan.where.string() ) ) );
				return;
			}
			QApplication::quit();
			return;
		}
#ifdef Q_OS_WIN
		if ( plan.kind == uninstall::Kind::WindowsPortable )
		{
			removePortable( plan );
			return;
		}
#endif

		// What takes root first, as it can be refused with nothing changed yet: the package, or programs in a prefix
		// this user cannot change.
		QStringList                        root;
		std::vector<std::filesystem::path> own = plan.programs;
		if ( !plan.package.empty() )
		{
			std::ranges::transform( plan.package, std::back_inserter( root ), qs );
		}
		else if ( std::ranges::any_of( own, uninstall::needsRoot ) )
		{
			root = { QStringLiteral( "rm" ), QStringLiteral( "-rf" ), QStringLiteral( "--" ) };
			std::ranges::transform( own, std::back_inserter( root ), []( const fs::path& path ) { return qs( path.string() ); } );
			own.clear();
		}
		if ( root.isEmpty() )
		{
			removeRest( plan, own );
			return;
		}
		const QString pkexec = QStandardPaths::findExecutable( QStringLiteral( "pkexec" ) );
		if ( pkexec.isEmpty() )
		{
			fail( QStringLiteral( "Removing it takes root, and pkexec is not installed. Run in a terminal: sudo %1" ).arg( root.join( ' ' ) ) );
			return;
		}
		// The program's path: pkexec runs it with a PATH of its own.
		const QString program = QStandardPaths::findExecutable( root.front() );
		if ( !program.isEmpty() )
		{
			root.front() = program;
		}
		auto* process = new QProcess( this );
		connect( process, &QProcess::finished, this, [this, process, plan, own]( int code, QProcess::ExitStatus status ) {
			process->deleteLater();
			if ( status != QProcess::NormalExit || code != 0 )
			{
				// pkexec: 126 when the password dialog was dismissed, 127 when it was refused.
				const QString output = QString::fromLocal8Bit( process->readAllStandardError() ).trimmed().section( '\n', -1 );
				fail( code == 126 || code == 127 ? QStringLiteral( "Cancelled: the password was not given. Nothing was removed." ) : QStringLiteral( "Uninstalling failed (%1). Nothing was removed." ).arg( output.isEmpty() ? QStringLiteral( "exit code %1" ).arg( code ) : output ) );
				return;
			}
			removeRest( plan, own );
		} );
		process->start( pkexec, root );
	}

	void UninstallOverlay::stopPrograms( std::function<void()> then )
	{
		// The daemon is asked over its pipe first, so it ends as when it is quit; whatever still runs after its answer is
		// ended.
		client_->call(
				"shutdown",
				"{}",
				[then = std::move( then )]( const json::Value*, const QString& ) {
					uninstall::stopPrograms( uninstall::Places::current(), std::chrono::seconds( 3 ) );
					then();
				},
				3000
		);
	}

	void UninstallOverlay::removeRest( const uninstall::Plan& plan, const std::vector<std::filesystem::path>& own )
	{
		stopPrograms( [this, plan, own] {
			std::vector<fs::path> rest = own;
			rest.insert( rest.end(), plan.integration.begin(), plan.integration.end() );
			rest.insert( rest.end(), plan.data.begin(), plan.data.end() );
			finish( plan, uninstall::remove( rest ) );
		} );
	}

	void UninstallOverlay::finish( const uninstall::Plan& plan, const std::vector<std::string>& failures )
	{
		finished_ = true;
		log::info( "uninstall: done, {} not removed", failures.size() );
		QString text = QStringLiteral( "Lexiglance was uninstalled." );
		if ( plan.kind == uninstall::Kind::BuildTree )
		{
			text += QStringLiteral( " The build in %1 stays: delete it yourself if it should go too." ).arg( qs( plan.where.string() ).toHtmlEscaped() );
		}
		if ( !failures.empty() )
		{
			text += QStringLiteral( "<br>These could not be removed:" ) + bulleted( failures );
		}
		text_->setText( text );
		erase_->hide();
		erase_note_->hide();
		status_->hide();
		cancel_->hide();
		uninstall_->setText( QStringLiteral( "Close" ) );
		uninstall_->setEnabled( true );
		uninstall_->setFocus();
		place();
	}

#ifdef Q_OS_WIN
	void UninstallOverlay::removePortable( const uninstall::Plan& plan )
	{
		const QString root = QDir::toNativeSeparators( qs( plan.where.string() ) );
		QStringList   remove;
		QStringList   emptied{ literal( root ) };
		for ( const std::vector<fs::path>* part : { &plan.programs, &plan.data } )
		{
			for ( const fs::path& path : *part )
			{
				remove << literal( QDir::toNativeSeparators( qs( path.string() ) ) );
				emptied << literal( QDir::toNativeSeparators( qs( path.parent_path().string() ) ) );
			}
		}
		emptied.removeDuplicates();
		const QString script = QDir::tempPath() + QStringLiteral( "/lexiglance-uninstall.ps1" );
		QFile         out( script );
		const QString text = QStringLiteral( "$root = %1\n$remove = @(%2)\n$emptied = @(%3)\n" ).arg( literal( root ), remove.join( ',' ), emptied.join( ',' ) );
		// The script's param() has to come first.
		QString body = QString::fromUtf8( removal_script );
		body.insert( body.indexOf( '\n' ) + 1, text );
		if ( !out.open( QIODevice::WriteOnly | QIODevice::Truncate ) || out.write( body.toUtf8() ) < 0 )
		{
			fail( QStringLiteral( "Cannot write %1. Nothing was removed." ).arg( script ) );
			return;
		}
		out.close();

		// Starting with Windows, and the Start menu shortcut, where they are this copy's.
		const auto here = [&]( const QString& path ) { return QDir::toNativeSeparators( path ).startsWith( root + '\\', Qt::CaseInsensitive ); };
		QSettings  run( QStringLiteral( "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" ), QSettings::NativeFormat );
		for ( const QString& name : { QStringLiteral( "Lexiglance" ), QStringLiteral( "Lexiglance tray" ) } )
		{
			if ( here( run.value( name ).toString().remove( '"' ) ) )
			{
				run.remove( name );
			}
		}
		const QString shortcut = QStandardPaths::writableLocation( QStandardPaths::ApplicationsLocation ) + QStringLiteral( "/Lexiglance.lnk" );
		if ( here( QFileInfo( shortcut ).symLinkTarget() ) )
		{
			QFile::remove( shortcut );
		}

		stopPrograms( [this, script, root] {
			const QString powershell = qEnvironmentVariable( "SystemRoot", QStringLiteral( "C:\\Windows" ) ) + QStringLiteral( "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" );
			if ( !QProcess::startDetached( powershell, { QStringLiteral( "-NoProfile" ), QStringLiteral( "-ExecutionPolicy" ), QStringLiteral( "Bypass" ), QStringLiteral( "-WindowStyle" ), QStringLiteral( "Hidden" ), QStringLiteral( "-File" ), QDir::toNativeSeparators( script ), QStringLiteral( "-Wait" ), QString::number( QCoreApplication::applicationPid() ) } ) )
			{
				fail( QStringLiteral( "Cannot start PowerShell to remove %1." ).arg( root ) );
				return;
			}
			QApplication::quit();
		} );
	}
#endif

} // namespace lexiglance::gui
