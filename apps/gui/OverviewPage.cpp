#include "OverviewPage.h"

#include "DaemonClient.h"
#include "DesktopEntry.h"
#include "Settings.h"
#include "UpdateGroup.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Version.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLocale>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace lexiglance::gui
{

	namespace
	{

#ifdef Q_OS_WIN
		QSettings runKey()
		{
			return { QStringLiteral( "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" ), QSettings::NativeFormat };
		}
#else
		// The desktop's autostart entry: XDG autostart, or a LaunchAgent on macOS.
		QString autostartFile()
		{
	#ifdef Q_OS_MACOS
			return QDir::homePath() + "/Library/LaunchAgents/io.github.mattfor.lexiglance.daemon.plist";
	#else
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-daemon.desktop";
	#endif
		}
#endif

		bool autostartEnabled()
		{
#ifdef Q_OS_WIN
			return runKey().contains( QStringLiteral( "Lexiglance" ) );
#else
			return QFile::exists( autostartFile() );
#endif
		}

		bool setAutostart( bool enabled )
		{
#ifdef Q_OS_WIN
			auto run = runKey();
			if ( enabled )
			{
				run.setValue( QStringLiteral( "Lexiglance" ), QStringLiteral( "\"%1\"" ).arg( QDir::toNativeSeparators( daemonExecutable() ) ) );
			}
			else
			{
				run.remove( QStringLiteral( "Lexiglance" ) );
			}
			run.sync();
			return run.status() == QSettings::NoError;
#else
			if ( !enabled )
			{
				return QFile::remove( autostartFile() ) || !QFile::exists( autostartFile() );
			}
			QDir().mkpath( QFileInfo( autostartFile() ).absolutePath() );
			QFile file( autostartFile() );
			if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
			{
				return false;
			}
	#ifdef Q_OS_MACOS
			const QString entry = QStringLiteral(
										  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
										  "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
										  "<plist version=\"1.0\"><dict><key>Label</key><string>io.github.mattfor.lexiglance.daemon</string>"
										  "<key>ProgramArguments</key><array><string>%1</string></array><key>RunAtLoad</key><true/></dict></plist>\n"
			)
			                              .arg( daemonExecutable().toHtmlEscaped() );
	#else
			const QString entry = QStringLiteral(
										  "[Desktop Entry]\nType=Application\nName=Lexiglance\nComment=System-wide pop-up dictionary\nExec=\"%1\"\n"
										  "Icon=lexiglance\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"
			)
			                              .arg( daemonExecutable() );
	#endif
			return file.write( entry.toUtf8() ) > 0;
#endif
		}

#ifndef Q_OS_MACOS
		// The settings application at login as well, in the tray: a second entry beside the daemon's.
	#ifdef Q_OS_WIN
		const QString tray_value = QStringLiteral( "Lexiglance tray" );
	#else
		QString trayAutostartFile()
		{
			return QStandardPaths::writableLocation( QStandardPaths::GenericConfigLocation ) + "/autostart/lexiglance-tray.desktop";
		}
	#endif

		bool trayAutostartEnabled()
		{
	#ifdef Q_OS_WIN
			return runKey().contains( tray_value );
	#else
			return QFile::exists( trayAutostartFile() );
	#endif
		}

		bool setTrayAutostart( bool enabled )
		{
	#ifdef Q_OS_WIN
			auto run = runKey();
			if ( enabled )
			{
				run.setValue( tray_value, QStringLiteral( "\"%1\" --tray" ).arg( QDir::toNativeSeparators( QCoreApplication::applicationFilePath() ) ) );
			}
			else
			{
				run.remove( tray_value );
			}
			run.sync();
			return run.status() == QSettings::NoError;
	#else
			if ( !enabled )
			{
				return QFile::remove( trayAutostartFile() ) || !QFile::exists( trayAutostartFile() );
			}
			// The AppImage file itself rather than its mount, which changes every run.
			const QString appimage = qEnvironmentVariable( "APPIMAGE" );
			const QString program  = appimage.isEmpty() ? QCoreApplication::applicationFilePath() : appimage;
			QDir().mkpath( QFileInfo( trayAutostartFile() ).absolutePath() );
			QFile file( trayAutostartFile() );
			if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
			{
				return false;
			}
			const QString entry = QStringLiteral(
										  "[Desktop Entry]\nType=Application\nName=Lexiglance (tray)\nComment=The Lexiglance settings application, in the tray\n"
										  "Exec=\"%1\" --tray\nIcon=lexiglance\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"
			)
			                              .arg( program );
			return file.write( entry.toUtf8() ) > 0;
	#endif
		}
#endif

		// A figure with its caption, for the status overview.
		QFrame* tile( QLabel* value, const QString& caption )
		{
			auto* frame = new QFrame();
			frame->setObjectName( QStringLiteral( "tile" ) );
			auto* layout = new QVBoxLayout( frame );
			layout->setContentsMargins( 12, 8, 12, 8 );
			layout->setSpacing( 2 );
			value->setObjectName( QStringLiteral( "tileValue" ) );
			value->setWordWrap( true );
			auto* label = new QLabel( caption );
			label->setObjectName( QStringLiteral( "tileCaption" ) );
			layout->addWidget( value );
			layout->addWidget( label );
			return frame;
		}

		// The state as a coloured pill: green while scanning, amber when paused, grey without a daemon.
		void showState( QLabel* state, const QString& text, const QString& background, const QString& foreground )
		{
			state->setText( text );
			state->setStyleSheet( QStringLiteral( "QLabel { background: %1; color: %2; border-radius: 11px; padding: 3px 12px; }" ).arg( background, foreground ) );
		}

		QLabel* heading( const QString& text )
		{
			auto* label = new QLabel( text );
			QFont font  = label->font();
			font.setPointSizeF( font.pointSizeF() * 1.9 );
			font.setBold( true );
			label->setFont( font );
			return label;
		}

		// How each kind of result is marked.
		struct Mark
		{
			QString symbol;
			QString colour;
		};

		Mark markOf( health::Severity status )
		{
			switch ( status )
			{
				case health::Severity::Info:
					return { .symbol = QStringLiteral( "ℹ" ), .colour = QStringLiteral( "#5b8def" ) };
				case health::Severity::Warning:
					return { .symbol = QStringLiteral( "▲" ), .colour = QStringLiteral( "#d19a1f" ) };
				case health::Severity::Error:
					return { .symbol = QStringLiteral( "✕" ), .colour = QStringLiteral( "#e0605a" ) };
				case health::Severity::Ok:
					break;
			}
			return { .symbol = QStringLiteral( "✓" ), .colour = QStringLiteral( "#3fa45b" ) };
		}

		QString fixLabel( const QString& action, bool connected )
		{
			if ( action == QStringLiteral( "restart" ) )
			{
				return connected ? QStringLiteral( "Restart" ) : QStringLiteral( "Start Lexiglance" );
			}
			if ( action == QStringLiteral( "resume" ) )
			{
				return QStringLiteral( "Resume scanning" );
			}
			if ( action == QStringLiteral( "enable-ocr" ) )
			{
				return QStringLiteral( "Turn OCR on" );
			}
			if ( action == QStringLiteral( "enable-accessibility" ) )
			{
				return QStringLiteral( "Turn it on" );
			}
			if ( action == QStringLiteral( "open-dictionaries" ) )
			{
				return QStringLiteral( "Open Dictionaries" );
			}
			if ( action == QStringLiteral( "open-scanning" ) )
			{
				return QStringLiteral( "Open Scanning" );
			}
			if ( action == QStringLiteral( "open-anki" ) )
			{
				return QStringLiteral( "Open Anki" );
			}
			return QStringLiteral( "Fix" );
		}

		// The daemon's own last words: its latest warnings and errors.
		QStringList logProblems( int count )
		{
			QFile file( qs( ( paths::stateDir() / "daemon.log" ).string() ) );
			if ( !file.open( QIODevice::ReadOnly ) )
			{
				return {};
			}
			file.seek( std::max<qint64>( 0, file.size() - ( qint64{ 64 } * 1024 ) ) );
			const QStringList lines = QString::fromUtf8( file.readAll() ).split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
			QStringList       found;
			for ( auto it = lines.rbegin(); it != lines.rend() && found.size() < count; ++it )
			{
				if ( it->contains( QStringLiteral( "] [error] " ) ) || it->contains( QStringLiteral( "] [warn] " ) ) )
				{
					found.prepend( *it );
				}
			}
			return found;
		}

	} // namespace

	OverviewPage::OverviewPage( Context context, QWidget* parent ) :
		Page( std::move( context ), parent ),
		state_( new QLabel() ),
		details_( new QLabel() ),
		trigger_( new QLabel() ),
		pause_( new QPushButton() ),
		restart_( new QPushButton( QStringLiteral( "Restart" ) ) ),
		autostart_( new QCheckBox( QStringLiteral( "Start Lexiglance automatically when I log in" ) ) ),
		autostart_tray_( new QCheckBox( QStringLiteral( "Also start this window, hidden in the tray" ) ) ),
		test_text_( new QLineEdit() ),
		test_result_( new QLabel() ),
		dictionaries_value_( new QLabel( QStringLiteral( "–" ) ) ),
		lookups_value_( new QLabel( QStringLiteral( "–" ) ) ),
		speed_value_( new QLabel( QStringLiteral( "–" ) ) ),
		capture_value_( new QLabel( QStringLiteral( "–" ) ) ),
		check_( new QPushButton( QStringLiteral( "Check health" ) ) ),
		health_summary_( new QLabel() ),
		show_passed_( new QCheckBox() ),
		health_rows_( new QVBoxLayout() ),
		trigger_state_( new QLabel() ),
		last_capture_( new QLabel() ),
		restart_timer_( new QTimer( this ) ),
		health_timer_( new QTimer( this ) )
	{
		auto* content = new QWidget();
		auto* layout  = new QVBoxLayout( content );
		layout->setContentsMargins( 28, 24, 28, 24 );
		layout->setSpacing( 14 );

		layout->addWidget( heading( QStringLiteral( "Lexiglance" ) ) );
		auto* tagline = new QLabel( QStringLiteral( "System-wide pop-up dictionary, version %1" ).arg( qs( version ) ) );
		tagline->setEnabled( false );
		layout->addWidget( tagline );

		auto* status_box    = new QGroupBox( QStringLiteral( "Status" ) );
		auto* status_layout = new QVBoxLayout( status_box );
		QFont state_font    = state_->font();
		state_font.setPointSizeF( state_font.pointSizeF() * 1.25 );
		state_font.setBold( true );
		state_->setFont( state_font );
		status_layout->addWidget( state_, 0, Qt::AlignLeft );
		status_layout->addWidget( trigger_ );
		auto* tiles = new QHBoxLayout();
		tiles->setSpacing( 10 );
		tiles->addWidget( tile( dictionaries_value_, QStringLiteral( "Dictionaries enabled" ) ), 1 );
		tiles->addWidget( tile( lookups_value_, QStringLiteral( "Lookups since start" ) ), 1 );
		tiles->addWidget( tile( speed_value_, QStringLiteral( "Average lookup" ) ), 1 );
		tiles->addWidget( tile( capture_value_, QStringLiteral( "Text capture" ) ), 2 );
		status_layout->addLayout( tiles );
		details_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		details_->setWordWrap( true );
		details_->setEnabled( false );
		status_layout->addWidget( details_ );

		auto* buttons = new QHBoxLayout();
		buttons->addWidget( pause_ );
		restart_->setToolTip( QStringLiteral( "Starts a new daemon that takes over from the running one, ending it if it does not answer." ) );
		buttons->addWidget( restart_ );
		buttons->addStretch( 1 );
		status_layout->addLayout( buttons );
		layout->addWidget( status_box );

		// Health: every part a lookup depends on, checked, with what fixes what is wrong.
		auto* health_box    = new QGroupBox( QStringLiteral( "Health" ) );
		auto* health_layout = new QVBoxLayout( health_box );
		auto* check_row     = new QHBoxLayout();
		check_->setProperty( "primary", true );
		check_row->addWidget( check_ );
		health_summary_->setWordWrap( true );
		check_row->addWidget( health_summary_, 1 );
		health_layout->addLayout( check_row );
		trigger_state_->setWordWrap( true );
		health_layout->addWidget( trigger_state_ );
		last_capture_->setWordWrap( true );
		last_capture_->setTextInteractionFlags( Qt::TextSelectableByMouse );
		last_capture_->setText( QStringLiteral( "Last lookup: none yet." ) );
		health_layout->addWidget( last_capture_ );
		health_rows_->setSpacing( 6 );
		health_layout->addLayout( health_rows_ );
		show_passed_->hide();
		health_layout->addWidget( show_passed_ );
		layout->addWidget( health_box );

		auto* test_box    = new QGroupBox( QStringLiteral( "Try it" ) );
		auto* test_layout = new QVBoxLayout( test_box );
		auto* hint        = new QLabel( QStringLiteral( "Hold the trigger keys and point at a word in any application. Scroll the popup with the wheel and click an entry to copy the word. "
		                                                "Or show a popup for this text:" ) );
		hint->setWordWrap( true );
		test_layout->addWidget( hint );
		auto* test_row  = new QHBoxLayout();
		auto* show_test = new QPushButton( QStringLiteral( "Show popup" ) );
		test_text_->setPlaceholderText( QStringLiteral( "A word or a sentence (empty: a word from your dictionaries)" ) );
		test_row->addWidget( test_text_, 1 );
		test_row->addWidget( show_test );
		test_layout->addLayout( test_row );
		test_result_->setEnabled( false );
		test_layout->addWidget( test_result_ );
		layout->addWidget( test_box );

		auto* startup_box    = new QGroupBox( QStringLiteral( "Startup" ) );
		auto* startup_layout = new QVBoxLayout( startup_box );
		autostart_->setChecked( autostartEnabled() );
		startup_layout->addWidget( autostart_ );
		// Under the option it belongs to.
		auto* tray_row = new QHBoxLayout();
		tray_row->addSpacing( 24 );
		tray_row->addWidget( autostart_tray_, 1 );
		startup_layout->addLayout( tray_row );
#ifdef Q_OS_MACOS
		autostart_tray_->hide();
#else
		// Without the daemon's autostart this window's would start the daemon anyway, so it goes too.
		if ( !autostart_->isChecked() && trayAutostartEnabled() )
		{
			setTrayAutostart( false );
		}
		autostart_tray_->setChecked( autostart_->isChecked() && trayAutostartEnabled() );
		autostart_tray_->setEnabled( autostart_->isChecked() );
		autostart_tray_->setToolTip( QStringLiteral( "At login this window starts as a tray icon: click the icon to open it, right-click it to pause scanning." ) );
#endif
#if !defined( Q_OS_WIN ) && !defined( Q_OS_MACOS )
		auto* menu_entry = new QCheckBox( QStringLiteral( "Show Lexiglance in the applications menu" ) );
		menu_entry->setChecked( desktop::menuEntryShown() );
		menu_entry->setToolTip( QStringLiteral( "A menu entry that opens these settings (with Check health and Search in its right-click menu)" ) );
		startup_layout->addWidget( menu_entry );
		connect( menu_entry, &QCheckBox::toggled, this, [this, menu_entry]( bool shown ) {
			QString error;
			if ( !desktop::setMenuEntry( shown, &error ) )
			{
				const QSignalBlocker blocker( menu_entry );
				menu_entry->setChecked( !shown );
				QMessageBox::warning( this, QStringLiteral( "Applications menu" ), error );
			}
		} );
#endif
		layout->addWidget( startup_box );
		layout->addWidget( new UpdateGroup() );

		auto* reset_box    = new QGroupBox( QStringLiteral( "Settings" ) );
		auto* reset_layout = new QHBoxLayout( reset_box );
		auto* reset_note   = new QLabel( QStringLiteral( "Every setting back to its default: the trigger, scanning, OCR, the popup's look, audio and Anki. Installed dictionaries and their order are kept." ) );
		reset_note->setWordWrap( true );
		reset_note->setEnabled( false );
		auto* reset_all = new QPushButton( QStringLiteral( "Reset all settings..." ) );
		reset_layout->addWidget( reset_note, 1 );
		reset_layout->addWidget( reset_all );
		layout->addWidget( reset_box );
		connect( reset_all, &QPushButton::clicked, this, [this] {
			if ( !confirm( this, QStringLiteral( "Reset all settings" ), QStringLiteral( "Every setting goes back to its default: the trigger, scanning, OCR, the popup's look, audio and Anki. Installed dictionaries and their order are kept." ), QStringLiteral( "Reset all" ) ) )
			{
				return;
			}
			config::Config defaults;
			defaults.dictionaries = settings().config().dictionaries;
			settings().replace( std::move( defaults ) );
		} );
		layout->addStretch( 1 );

		auto* scroll = new QScrollArea();
		scroll->setWidget( content );
		scroll->setWidgetResizable( true );
		scroll->setFrameShape( QFrame::NoFrame );
		scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
		auto* outer = new QVBoxLayout( this );
		outer->setContentsMargins( 0, 0, 0, 0 );
		outer->addWidget( scroll );

		connect( pause_, &QPushButton::clicked, this, [this] { client().call( "scan.pause", paused_ ? "{\"paused\":false}" : "{\"paused\":true}" ); } );
		connect( restart_, &QPushButton::clicked, this, [this] { restart(); } );
		connect( check_, &QPushButton::clicked, this, [this] { checkHealth( true ); } );
		connect( show_passed_, &QCheckBox::toggled, this, [this] { showChecks( checks_ ); } );
		connect( autostart_, &QCheckBox::toggled, this, [this]( bool enabled ) {
			setAutostart( enabled );
			autostart_tray_->setEnabled( enabled );
			if ( !enabled )
			{
				autostart_tray_->setChecked( false );
			}
		} );
#ifndef Q_OS_MACOS
		connect( autostart_tray_, &QCheckBox::toggled, this, []( bool enabled ) { setTrayAutostart( enabled ); } );
#endif

		restart_timer_->setInterval( 300 );
		connect( restart_timer_, &QTimer::timeout, this, [this] {
			if ( QDateTime::currentMSecsSinceEpoch() - restart_started_ > 15000 )
			{
				finishRestart( false, QStringLiteral( "No new daemon answered within 15 seconds." ) );
				return;
			}
			if ( !client().connected() )
			{
				return;
			}
			client().call(
					"status",
					"{}",
					[this]( const json::Value* status, const QString& ) {
						if ( !restarting_ || status == nullptr )
						{
							return;
						}
						// The old daemon may still answer for a moment; only a new one counts.
						const auto pid = ( *status )["pid"].asInt();
						if ( pid != 0 && pid != restart_from_ )
						{
							client().setDaemonPid( pid );
							finishRestart( true, QStringLiteral( "Restarted: the daemon now runs as process %1." ).arg( pid ) );
							setStatus( *status );
						}
					},
					2000
			);
		} );

		const auto show = [this] {
			// Delay so the pointer can be moved away from the button and the popup lands next to it.
			test_result_->setText( QStringLiteral( "The popup appears at the mouse pointer..." ) );
			QTimer::singleShot( 250, this, [this] {
				json::Writer params;
				params.beginObject().field( "text", ss( test_text_->text() ) ).endObject();
				client().call( "popup.show", params.take(), [this]( const json::Value*, const QString& error ) { test_result_->setText( error ); } );
			} );
		};
		connect( show_test, &QPushButton::clicked, this, show );
		connect( test_text_, &QLineEdit::returnPressed, this, show );

		client().onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( name == "trigger.changed" )
			{
				setTrigger( params["held"].asBool() );
			}
			else if ( name == "capture.result" )
			{
				const bool found = params["found"].asBool();
				last_capture_->setText( QStringLiteral( "Last lookup: <span style=\"color:%1\">%2</span> <span style=\"color:gray\">(%3)</span>" )
				                                .arg( found ? QStringLiteral( "#3fa45b" ) : QStringLiteral( "#d19a1f" ), qs( params["summary"].asString() ).toHtmlEscaped(), qs( params["where"].asString() ).toHtmlEscaped() ) );
			}
			// What the health depends on changed: checked again once it settles, or when the page is next shown.
			else if ( name == "dictionaries.changed" || name == "config.changed" || name == "status.changed" )
			{
				if ( isVisible() )
				{
					health_timer_->start();
				}
				else
				{
					health_stale_ = true;
				}
			}
		} );
		health_timer_->setSingleShot( true );
		health_timer_->setInterval( 1200 );
		connect( health_timer_, &QTimer::timeout, this, [this] {
			if ( checking_ || restarting_ )
			{
				health_timer_->start();
				return;
			}
			checkHealth( false );
		} );

		state_->setText( QStringLiteral( "Connecting..." ) );
		setTrigger( false );
		client().onConnection( [this]( bool connected ) {
			if ( !connected && !restarting_ )
			{
				showState( state_, QStringLiteral( "⏻  Not running" ), QStringLiteral( "#4a4c52" ), QStringLiteral( "#e4e5e8" ) );
				details_->setText( QStringLiteral( "The daemon is not running: nothing is looked up. Start it with the Restart button or the one in the status bar." ) );
				QTimer::singleShot( 2500, this, [this] {
					if ( !client().connected() && !restarting_ )
					{
						showChecks( localChecks() );
					}
				} );
			}
			pause_->setEnabled( connected );
			restart_->setText( connected ? QStringLiteral( "Restart" ) : QStringLiteral( "Start" ) );
			if ( connected && !restarting_ )
			{
				// A quiet first check, so problems show without asking.
				QTimer::singleShot( 1500, this, [this] {
					if ( checks_.empty() )
					{
						checkHealth( false );
					}
				} );
			}
		} );
	}

	void OverviewPage::refresh()
	{
		trigger_->setText( QStringLiteral( "Trigger: <b>%1</b>" ).arg( chordText( settings().config().scan.trigger ).toHtmlEscaped() ) );
		setTrigger( false );
	}

	void OverviewPage::showEvent( QShowEvent* event )
	{
		Page::showEvent( event );
		if ( health_stale_ )
		{
			health_stale_ = false;
			health_timer_->start();
		}
	}

	void OverviewPage::setTrigger( bool held )
	{
		const QString chord = chordText( settings().config().scan.trigger ).toHtmlEscaped();
		if ( held )
		{
			trigger_state_->setText( QStringLiteral( "<span style=\"color:#3fa45b\">●</span> <b>%1</b> is held: Lexiglance sees the trigger." ).arg( chord ) );
		}
		else
		{
			trigger_state_->setText( QStringLiteral( "<span style=\"color:gray\">○</span> Test the trigger: hold <b>%1</b> and this turns green." ).arg( chord ) );
		}
	}

	void OverviewPage::setStatus( const json::Value& status )
	{
		// While restarting, the old daemon's last answers do not change the state.
		if ( restarting_ )
		{
			return;
		}
		paused_ = status["paused"].asBool();
		if ( paused_ )
		{
			showState( state_, QStringLiteral( "⏸  Paused" ), QStringLiteral( "#5a4320" ), QStringLiteral( "#ffd79a" ) );
		}
		else
		{
			showState( state_, QStringLiteral( "●  Scanning" ), QStringLiteral( "#2f5f3a" ), QStringLiteral( "#bff0c8" ) );
		}
		pause_->setText( paused_ ? QStringLiteral( "Resume scanning" ) : QStringLiteral( "Pause scanning" ) );
		pause_->setProperty( "primary", paused_ );
		pause_->style()->unpolish( pause_ );
		pause_->style()->polish( pause_ );

		const auto lookups = status["lookups"].asInt();
		dictionaries_value_->setText( QString::number( status["dictionaries"].asInt() ) );
		lookups_value_->setText( QLocale().toString( static_cast<qlonglong>( lookups ) ) );
		speed_value_->setText( lookups > 0 ? QStringLiteral( "%1 µs" ).arg( status["average_lookup_us"].asDouble(), 0, 'f', 1 ) : QStringLiteral( "–" ) );
		capture_value_->setText( qs( status["capture"].asString( "unavailable" ) ) );
		details_->setText( QStringLiteral( "Daemon %1 (pid %2) · %3 desktop · scale %4×" )
		                           .arg( qs( status["version"].asString() ) )
		                           .arg( status["pid"].asInt() )
		                           .arg( qs( status["backend"].asString() ) )
		                           .arg( status["scale"].asDouble(), 0, 'g', 3 ) );
	}

	void OverviewPage::checkHealth( bool interactive )
	{
		if ( checking_ )
		{
			return;
		}
		if ( !client().connected() )
		{
			showChecks( localChecks() );
			return;
		}
		checking_ = true;
		check_->setEnabled( false );
		health_summary_->setText( QStringLiteral( "Checking..." ) );
		client().call(
				"health",
				interactive ? R"({"interactive":true})" : "{}",
				[this]( const json::Value* result, const QString& error ) {
					checking_ = false;
					check_->setEnabled( true );
					if ( result == nullptr )
					{
						auto checks = localChecks();
						checks.insert( checks.begin(), { .id = "health", .title = "Health check", .status = health::Severity::Error, .detail = ss( QStringLiteral( "The daemon did not answer the health check (%1)." ).arg( error ) ), .fix = "restart" } );
						showChecks( checks );
						return;
					}
					showChecks( health::read( *result ) );
				},
				30000
		);
	}

	std::vector<health::Check> OverviewPage::localChecks()
	{
		std::vector<health::Check> checks;
		const QString              program = daemonExecutable();
		if ( !QFileInfo( program ).isExecutable() && QStandardPaths::findExecutable( program ).isEmpty() )
		{
			checks.push_back(
					{ .id     = "program",
			          .title  = "Daemon program",
			          .status = health::Severity::Error,
			          .detail = ss( QStringLiteral( "lexiglanced was not found next to the settings application or on PATH (%1)." ).arg( program ) ) }
			);
		}
		const auto running = process::othersNamed( "lexiglanced" );
		if ( running.empty() )
		{
			checks.push_back( { .id = "daemon", .title = "Lexiglance daemon", .status = health::Severity::Error, .detail = "Lexiglance is not running, so nothing is looked up.", .fix = "restart" } );
		}
		else
		{
			QStringList pids;
			for ( const int pid : running )
			{
				pids << QString::number( pid );
			}
			checks.push_back(
					{ .id     = "daemon",
			          .title  = "Lexiglance daemon",
			          .status = health::Severity::Error,
			          .detail = ss( QStringLiteral( "Lexiglance is running (process %1) but does not answer. Restarting ends it and starts a new one." ).arg( pids.join( QStringLiteral( ", " ) ) ) ),
			          .fix    = "restart" }
			);
		}
		const QStringList problems = logProblems( 3 );
		if ( !problems.isEmpty() )
		{
			checks.push_back( { .id = "log", .title = "The daemon's log", .status = health::Severity::Info, .detail = ss( problems.join( QLatin1Char( '\n' ) ) ) } );
		}
		return checks;
	}

	void OverviewPage::showChecks( const std::vector<health::Check>& checks )
	{
		checks_ = checks;
		while ( QLayoutItem* item = health_rows_->takeAt( 0 ) )
		{
			delete item->widget();
			delete item;
		}

		int errors   = 0;
		int warnings = 0;
		int passed   = 0;
		for ( const auto& check : checks )
		{
			errors += check.status == health::Severity::Error ? 1 : 0;
			warnings += check.status == health::Severity::Warning ? 1 : 0;
			passed += check.status == health::Severity::Ok ? 1 : 0;
			if ( check.status == health::Severity::Ok && !show_passed_->isChecked() )
			{
				continue;
			}

			auto* row    = new QFrame();
			auto* layout = new QHBoxLayout( row );
			row->setObjectName( QStringLiteral( "healthRow" ) );
			layout->setContentsMargins( 10, 6, 10, 6 );
			const Mark mark = markOf( check.status );
			auto*      icon = new QLabel( mark.symbol );
			icon->setStyleSheet( QStringLiteral( "QLabel { color: %1; font-weight: 700; }" ).arg( mark.colour ) );
			icon->setFixedWidth( 18 );
			icon->setAlignment( Qt::AlignTop | Qt::AlignHCenter );
			layout->addWidget( icon, 0, Qt::AlignTop );
			auto* text = new QLabel( QStringLiteral( "<b>%1</b><br>%2" ).arg( qs( check.title ).toHtmlEscaped(), qs( check.detail ).toHtmlEscaped().replace( QLatin1Char( '\n' ), QStringLiteral( "<br>" ) ) ) );
			text->setWordWrap( true );
			text->setTextInteractionFlags( Qt::TextSelectableByMouse );
			layout->addWidget( text, 1 );
			if ( !check.fix.empty() )
			{
				const QString action = qs( check.fix );
				auto*         button = new QPushButton( fixLabel( action, client().connected() ) );
				connect( button, &QPushButton::clicked, this, [this, action] { fix( action ); } );
				layout->addWidget( button, 0, Qt::AlignVCenter );
			}
			health_rows_->addWidget( row );
		}

		QString summary;
		if ( errors > 0 )
		{
			summary = QStringLiteral( "<span style=\"color:#e0605a\"><b>✕ %1 %2 attention</b></span>" ).arg( errors ).arg( errors == 1 ? QStringLiteral( "problem needs" ) : QStringLiteral( "problems need" ) );
		}
		else if ( warnings > 0 )
		{
			summary = QStringLiteral( "<span style=\"color:#d19a1f\"><b>▲ %1 %2</b></span>" ).arg( warnings ).arg( warnings == 1 ? QStringLiteral( "warning" ) : QStringLiteral( "warnings" ) );
		}
		else if ( !checks.empty() )
		{
			summary = QStringLiteral( "<span style=\"color:#3fa45b\"><b>✓ Everything works</b></span>" );
		}
		if ( passed > 0 )
		{
			summary += QStringLiteral( " <span style=\"color:gray\">· %1 checks passed · %2</span>" ).arg( passed ).arg( QDateTime::currentDateTime().toString( QStringLiteral( "HH:mm:ss" ) ) );
		}
		health_summary_->setText( summary );
		show_passed_->setVisible( passed > 0 );
		show_passed_->setText( QStringLiteral( "Show the %1 passed checks too" ).arg( passed ) );
	}

	void OverviewPage::fix( const QString& action )
	{
		const auto recheck = [this] { QTimer::singleShot( 2000, this, [this] { checkHealth( true ); } ); };
		if ( action == QStringLiteral( "restart" ) )
		{
			restart();
		}
		else if ( action == QStringLiteral( "resume" ) )
		{
			client().call( "scan.pause", R"({"paused":false})", [recheck]( const json::Value*, const QString& ) { recheck(); } );
		}
		else if ( action == QStringLiteral( "enable-ocr" ) )
		{
			settings().config().scan.ocr = config::OcrMode::Fallback;
			settings().commit();
			recheck();
		}
		else if ( action == QStringLiteral( "enable-accessibility" ) )
		{
			settings().config().scan.accessibility = true;
			settings().commit();
			recheck();
		}
		else if ( action.startsWith( QStringLiteral( "open-" ) ) )
		{
			showPage( action.mid( 5 ) );
		}
	}

	void OverviewPage::restart()
	{
		if ( restarting_ )
		{
			return;
		}
		restarting_      = true;
		restart_from_    = client().connected() ? client().daemonPid() : 0;
		restart_started_ = QDateTime::currentMSecsSinceEpoch();
		restart_->setEnabled( false );
		showState( state_, QStringLiteral( "⟳  Restarting..." ), QStringLiteral( "#5a4320" ), QStringLiteral( "#ffd79a" ) );
		details_->setText( QStringLiteral( "A new daemon is starting; it takes over from the running one." ) );
		if ( !client().startDaemon( true ) )
		{
			finishRestart( false, QStringLiteral( "The daemon program could not be started (%1)." ).arg( daemonExecutable() ) );
			return;
		}
		restart_timer_->start();
	}

	void OverviewPage::finishRestart( bool ok, const QString& message )
	{
		restarting_ = false;
		restart_timer_->stop();
		restart_->setEnabled( true );
		details_->setText( message );
		if ( ok )
		{
			QTimer::singleShot( 800, this, [this] { checkHealth( true ); } );
			return;
		}
		showState( state_, QStringLiteral( "✕  Restart failed" ), QStringLiteral( "#5c2b2b" ), QStringLiteral( "#ffb3b3" ) );
		showChecks( localChecks() );
	}

} // namespace lexiglance::gui
