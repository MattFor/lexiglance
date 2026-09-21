#include "MainWindow.h"

#include "AboutPage.h"
#include "AnkiPage.h"
#include "AppearancePage.h"
#include "DictionariesPage.h"
#include "OverviewPage.h"
#include "ScanningPage.h"
#include "SearchPage.h"
#include "SetupWizard.h"
#include "StatisticsPage.h"
#include "TranslationPage.h"

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Version.h>

#include <QEvent>
#include <QComboBox>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPalette>
#include <QScrollBar>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#endif

namespace lexiglance::gui
{

	namespace
	{

		// The look on top of the palette: groups as cards, rounded fields, one accent colour for what matters.
		QString applicationStyle( bool dark )
		{
			const QString card    = dark ? QStringLiteral( "#2b2c31" ) : QStringLiteral( "#ffffff" );
			const QString field   = dark ? QStringLiteral( "#1c1d20" ) : QStringLiteral( "#ffffff" );
			const QString border  = dark ? QStringLiteral( "#3a3c42" ) : QStringLiteral( "#d6d9de" );
			const QString hover   = dark ? QStringLiteral( "#34363b" ) : QStringLiteral( "#e6e8ec" );
			const QString text    = dark ? QStringLiteral( "#dddee1" ) : QStringLiteral( "#1d1e21" );
			const QString muted   = dark ? QStringLiteral( "#8a8c92" ) : QStringLiteral( "#6b6f76" );
			const QString sidebar = dark ? QStringLiteral( "#1f2023" ) : QStringLiteral( "#eceef1" );
			const QString accent  = QStringLiteral( "#3578d2" );
			return QStringLiteral( R"(
QGroupBox { background: %1; border: 1px solid %3; border-radius: 10px; margin-top: 1.6em; padding: 10px 12px 12px 12px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 4px; padding: 0 2px; color: %5; font-weight: 600; }
QPushButton { background: %4; border: 1px solid %3; border-radius: 6px; padding: 5px 14px; }
QPushButton:hover { border-color: %8; }
QPushButton:pressed { background: %3; }
QPushButton:disabled { color: %6; }
QPushButton[primary="true"] { background: %8; border-color: %8; color: #ffffff; }
QPushButton[primary="true"]:disabled { background: %4; border-color: %3; color: %6; }
QPushButton[pill="true"] { border-radius: 10px; padding: 3px 12px; }
QPushButton[pill="true"]:checked { background: %8; border-color: %8; color: #ffffff; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextBrowser, QTreeWidget, QTableWidget { background: %2; border: 1px solid %3; border-radius: 6px; padding: 3px 6px; selection-background-color: %8; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color: %8; }
QHeaderView::section { background: %1; border: none; border-bottom: 1px solid %3; padding: 4px 6px; color: %6; }
QCheckBox::indicator, QTreeView::indicator { width: 15px; height: 15px; border: 1px solid %3; border-radius: 4px; background: %2; }
QCheckBox::indicator:checked, QTreeView::indicator:checked { background: %8; border-color: %8; image: url(:/lexiglance/check.png); }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 18px; border: none; background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; }
QAbstractSpinBox::up-arrow { image: url(:/lexiglance/up.png); width: 10px; height: 10px; }
QAbstractSpinBox::down-arrow { image: url(:/lexiglance/down.png); width: 10px; height: 10px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(:/lexiglance/down.png); width: 10px; height: 10px; }
QWidget#sidebar { background: %7; }
QPushButton#sidebarButton { background: transparent; border: none; border-radius: 7px; margin: 0 8px; padding: 8px 10px 8px 14px; text-align: left; }
QPushButton#sidebarButton:hover { background: %4; }
QPushButton#donateButton { background: #d6457a; border: none; border-radius: 16px; color: #ffffff; font-weight: 600; padding: 8px 20px; }
QPushButton#donateButton:hover { background: #e65d8f; }
QPushButton#donateButton:pressed { background: #b93a69; }
QFrame#helpCard { background: %1; border: 1px solid %3; border-radius: 14px; }
QListWidget#navigation { background: %7; border: none; padding: 8px; }
QListWidget#navigation::item { padding: 6px 10px; border-radius: 7px; margin: 1px 0; }
QListWidget#navigation::item:hover { background: %4; }
QListWidget#navigation::item:selected { background: %4; color: %5; border-left: 3px solid %8; }
QFrame#tile { background: %2; border: 1px solid %3; border-radius: 8px; }
QFrame#healthRow { background: %2; border: 1px solid %3; border-radius: 8px; }
QLabel#tileValue { font-size: 17px; font-weight: 600; }
QLabel#tileCaption { color: %6; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %3; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QToolTip { background: %1; color: %5; border: 1px solid %3; padding: 4px; }
QStatusBar { background: %7; border-top: 1px solid %3; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 3px 8px; color: %6; }
)" )
			        .arg( card, field, border, hover, text, muted, sidebar, accent );
		}

	} // namespace

	namespace
	{

		// The wheel over a spin box, a drop-down or a slider scrolls the page; it changes the value only once the field
		// was clicked, so scrolling through the settings never changes one by accident.
		class WheelGuard final : public QObject
		{
		public:
			using QObject::QObject;

			bool eventFilter( QObject* watched, QEvent* event ) override
			{
				if ( event->type() != QEvent::Wheel && event->type() != QEvent::Polish )
				{
					return false;
				}
				auto* widget = qobject_cast<QWidget*>( watched );
				// Scroll bars are sliders too, but they are what scrolls the page: never held back.
				if ( widget == nullptr || qobject_cast<QScrollBar*>( widget ) != nullptr || ( qobject_cast<QAbstractSpinBox*>( widget ) == nullptr && qobject_cast<QComboBox*>( widget ) == nullptr && qobject_cast<QAbstractSlider*>( widget ) == nullptr ) )
				{
					return false;
				}
				if ( event->type() == QEvent::Polish )
				{
					// No focus from the wheel itself.
					widget->setFocusPolicy( Qt::StrongFocus );
					return false;
				}
				if ( widget->hasFocus() )
				{
					return false;
				}
				// Ignored, it goes on to the page, which scrolls.
				event->ignore();
				return true;
			}
		};

	} // namespace

	MainWindow::MainWindow() :
		client_( new DaemonClient( this ) ),
		settings_( std::make_unique<Settings>( *client_ ) ),
		navigation_( new QListWidget() ),
		pages_( new QStackedWidget() ),
		connection_( new QLabel() ),
		summary_( new QLabel() ),
		start_button_( new QPushButton( QStringLiteral( "Start Lexiglance" ) ) )
	{
		qApp->installEventFilter( new WheelGuard( this ) );
		start_button_->setProperty( "primary", true );
		setWindowTitle( QStringLiteral( "Lexiglance" ) );
		// The installed icon, or the program's own copy (a build tree or an AppImage has none installed), also as a PNG for
		// where Qt draws no SVG (Windows).
		QIcon own( QStringLiteral( ":/lexiglance/lexiglance.svg" ) );
		own.addFile( QStringLiteral( ":/lexiglance/lexiglance.png" ) );
		setWindowIcon( QIcon::fromTheme( QStringLiteral( "lexiglance" ), own ) );
		resize( 1080, 720 );

		// Always, as it also brings in the bundled icons (the release AppImage is built with Qt 6.4, which cannot tell the
		// desktop's colour scheme: light until the daemon says otherwise).
		bool dark = false;
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
		const auto scheme    = QGuiApplication::styleHints()->colorScheme();
		palette_from_daemon_ = scheme == Qt::ColorScheme::Unknown;
		dark                 = scheme == Qt::ColorScheme::Dark;
#endif
		applyPalette( dark );

		navigation_->setObjectName( QStringLiteral( "navigation" ) );
		navigation_->setIconSize( QSize( 20, 20 ) );
		navigation_->setSpacing( 2 );
		navigation_->setFrameShape( QFrame::NoFrame );

		// The pages, and the help at the bottom left.
		auto* sidebar = new QWidget();
		sidebar->setObjectName( QStringLiteral( "sidebar" ) );
		sidebar->setAttribute( Qt::WA_StyledBackground, true );
		sidebar->setFixedWidth( 190 );
		auto* sidebar_layout = new QVBoxLayout( sidebar );
		sidebar_layout->setContentsMargins( 0, 0, 0, 10 );
		sidebar_layout->setSpacing( 0 );
		sidebar_layout->addWidget( navigation_, 1 );
		auto* help = new QPushButton( QIcon::fromTheme( QStringLiteral( "help-contents" ) ), QStringLiteral( "Help" ) );
		help->setObjectName( QStringLiteral( "sidebarButton" ) );
		help->setIconSize( QSize( 20, 20 ) );
		help->setToolTip( QStringLiteral( "What Lexiglance can do, and where" ) );
		sidebar_layout->addWidget( help );
		connect( help, &QPushButton::clicked, this, [this] { showHelp(); } );

		auto* central = new QWidget();
		auto* layout  = new QHBoxLayout( central );
		layout->setContentsMargins( 0, 0, 0, 0 );
		layout->setSpacing( 0 );
		layout->addWidget( sidebar );
		layout->addWidget( pages_, 1 );
		setCentralWidget( central );

		const Context context{ .client = client_, .settings = settings_.get(), .show_page = [this]( const QString& page ) { showPage( page ); }, .show_summary = [this]( const QString& text ) { summary_->setText( text ); } };
		addPage( new OverviewPage( context ), QStringLiteral( "Overview" ), QStringLiteral( "dialog-information" ) );
		addPage( new DictionariesPage( context ), QStringLiteral( "Dictionaries" ), QStringLiteral( "accessories-dictionary" ) );
		addPage( new ScanningPage( context ), QStringLiteral( "Scanning" ), QStringLiteral( "input-keyboard" ) );
		addPage( new TranslationPage( context ), QStringLiteral( "Translation" ), QStringLiteral( "preferences-desktop-locale" ) );
		addPage( new AppearancePage( context ), QStringLiteral( "Appearance" ), QStringLiteral( "preferences-desktop-theme" ) );
		addPage( new SearchPage( context ), QStringLiteral( "Search" ), QStringLiteral( "edit-find" ) );
		addPage( new AnkiPage( context ), QStringLiteral( "Anki" ), QStringLiteral( "document-send" ) );
		addPage( new StatisticsPage( context ), QStringLiteral( "Statistics" ), QStringLiteral( "office-chart-bar" ) );
		addPage( new AboutPage( context ), QStringLiteral( "About" ), QStringLiteral( "help-about" ) );

		connect( navigation_, &QListWidget::currentRowChanged, this, [this]( int row ) {
			pages_->setCurrentIndex( row );
			if ( row >= 0 && static_cast<std::size_t>( row ) < page_list_.size() )
			{
				page_list_[static_cast<std::size_t>( row )]->activated();
			}
		} );
		navigation_->setCurrentRow( 0 );

		statusBar()->addWidget( connection_, 1 );
		statusBar()->addPermanentWidget( summary_ );
		statusBar()->addPermanentWidget( start_button_ );
		// Nothing answers: a daemon that hangs is replaced as well as a missing one started.
		connect( start_button_, &QPushButton::clicked, this, [this] { client_->startDaemon( true ); } );

		settings_->onChanged( [this] {
			for ( Page* page : page_list_ )
			{
				page->refresh();
			}
		} );

		client_->onConnection( [this]( bool connected ) {
			connection_->setText( connected ? QStringLiteral( "Connected to the Lexiglance daemon" ) : QStringLiteral( "The Lexiglance daemon is not running" ) );
			start_button_->setVisible( !connected );
			if ( connected )
			{
				client_->call( "status", "{}", [this]( const json::Value* status, const QString& ) {
					if ( status != nullptr )
					{
						updateStatus( *status );
					}
				} );
			}
		} );
		client_->onEvent( [this]( std::string_view name, const json::Value& params ) {
			if ( name == "status.changed" )
			{
				updateStatus( params );
			}
		} );

		setupTray();
		connection_->setText( QStringLiteral( "Connecting..." ) );
		start_button_->hide();
		client_->connectNow();

		// Starting the settings application starts the dictionary as well.
		QTimer::singleShot( 600, this, [this] {
			if ( !client_->connected() && !tried_start_ )
			{
				tried_start_ = true;
				client_->startDaemon();
			}
		} );

		auto* poll = new QTimer( this );
		poll->setInterval( 3000 );
		connect( poll, &QTimer::timeout, this, [this] {
			client_->call( "status", "{}", [this]( const json::Value* status, const QString& ) {
				if ( status != nullptr )
				{
					updateStatus( *status );
				}
			} );
		} );
		poll->start();
	}

	void MainWindow::addPage( Page* page, const QString& title, const QString& icon )
	{
		page_list_.push_back( page );
		pages_->addWidget( page );
		auto* item = new QListWidgetItem( QIcon::fromTheme( icon ), title );
		item->setSizeHint( QSize( 0, 38 ) );
		navigation_->addItem( item );
	}

	namespace
	{

		// Windows gives the foreground only to the program last used: one the setup started (after an update, or on the
		// first start after installing) would only flash in the taskbar. Sharing the input of the window in front for a
		// moment lifts that.
		void bringToFront( QWidget* window )
		{
#ifdef _WIN32
			const auto  target = reinterpret_cast<HWND>( window->winId() );
			const HWND  front  = GetForegroundWindow();
			const DWORD theirs = front != nullptr ? GetWindowThreadProcessId( front, nullptr ) : 0;
			const DWORD ours   = GetCurrentThreadId();
			const bool  joined = theirs != 0 && theirs != ours && AttachThreadInput( theirs, ours, TRUE ) != 0;
			BringWindowToTop( target );
			SetForegroundWindow( target );
			if ( joined )
			{
				AttachThreadInput( theirs, ours, FALSE );
			}
#else
			( void )window;
#endif
		}

	} // namespace

	void MainWindow::present()
	{
		show();
		setWindowState( ( windowState() & ~Qt::WindowMinimized ) | Qt::WindowActive );
		raise();
		activateWindow();
		// Back from an update (UpdateGroup notes it): what changed, once, the first time the window is shown after it.
		auto memory = applicationMemory();
		if ( const QString updated = memory.value( QStringLiteral( "update/changes" ) ).toString(); !updated.isEmpty() )
		{
			memory.remove( QStringLiteral( "update/changes" ) );
			bringToFront( this );
			// After what opens with the window (the setup wizard after an update), so it is what is seen first.
			QTimer::singleShot( 0, this, [this, updated] { showChanges( updated ); } );
		}
	}

	void MainWindow::showChanges( const QString& version )
	{
		if ( changes_ == nullptr )
		{
			changes_ = new ChangesOverlay( this );
		}
		changes_->open( version );
	}

	void MainWindow::showPage( const QString& name )
	{
		if ( name.toLower() == QStringLiteral( "help" ) )
		{
			present();
			showHelp();
			return;
		}
		// In the order they were added above, which is the order of the sidebar.
		static const QStringList pages{ QStringLiteral( "overview" ), QStringLiteral( "dictionaries" ), QStringLiteral( "scanning" ), QStringLiteral( "translation" ), QStringLiteral( "appearance" ), QStringLiteral( "search" ), QStringLiteral( "anki" ), QStringLiteral( "statistics" ), QStringLiteral( "about" ) };
		if ( const auto index = pages.indexOf( name.toLower() ); index >= 0 )
		{
			navigation_->setCurrentRow( static_cast<int>( index ) );
		}
		present();
	}

	void MainWindow::search( const QString& text )
	{
		showPage( QStringLiteral( "search" ) );
		for ( Page* page : page_list_ )
		{
			if ( auto* search = dynamic_cast<SearchPage*>( page ) )
			{
				search->setQuery( text );
				return;
			}
		}
	}

	void MainWindow::showHelp( bool first )
	{
		if ( help_ == nullptr )
		{
			help_ = new HelpOverlay(
					[this]( const QString& page ) { showPage( page ); },
					[this] { showSetup( 0, true ); },
					[this] { showChanges( qs( version ) ); },
					this
			);
		}
		const auto& translation = settings_->config().translation;
		help_->open( chordText( settings_->config().scan.trigger ), translation.enabled && !translation.sentence_key.empty() ? qs( config::displayName( translation.sentence_key ) ) : QString(), first );
	}

	void MainWindow::showSetup( int lock_seconds, bool reinstall, bool required )
	{
		present();
		if ( setup_ == nullptr )
		{
			const Context context{ .client = client_, .settings = settings_.get(), .show_page = [this]( const QString& page ) { showPage( page ); }, .show_summary = [this]( const QString& text ) { summary_->setText( text ); } };
			setup_ = new SetupWizard( context, {}, this );
		}
		setup_->open( lock_seconds, reinstall, required );
	}

	void MainWindow::welcome()
	{
		auto       memory       = applicationMemory();
		const bool setup_done   = memory.value( QStringLiteral( "setup/completed" ) ).toBool();
		const bool after_update = memory.value( QStringLiteral( "setup/after_update" ) ).toBool();
		if ( after_update )
		{
			memory.setValue( QStringLiteral( "setup/after_update" ), false );
		}
		// Installs that already saw the old help card: do not force the new wizard once.
		if ( !setup_done && memory.value( QStringLiteral( "help/shown" ) ).toBool() && !after_update )
		{
			memory.setValue( QStringLiteral( "setup/completed" ), true );
			return;
		}
		// First start, or back from an automatic update: the frictionless language / OCR / dictionary setup.
		if ( !setup_done || after_update )
		{
			showSetup( 5, false, !setup_done );
			// Opened by the setup that just installed it: in front, not flashing in the taskbar.
			bringToFront( this );
			return;
		}
	}

	void MainWindow::setupTray()
	{
		if ( !QSystemTrayIcon::isSystemTrayAvailable() )
		{
			// At login the tray can come up after this program (started with --tray): look again for a minute.
			if ( ++tray_tries_ <= 30 )
			{
				QTimer::singleShot( 2000, this, [this] { setupTray(); } );
			}
			return;
		}
		tray_ = new QSystemTrayIcon( windowIcon(), this );
		tray_->setToolTip( QStringLiteral( "Lexiglance" ) );

		auto* menu    = new QMenu( this );
		pause_action_ = menu->addAction( QStringLiteral( "Pause scanning" ) );
		pause_action_->setCheckable( true );
		connect( pause_action_, &QAction::triggered, this, [this]( bool paused ) { client_->call( "scan.pause", paused ? "{\"paused\":true}" : "{\"paused\":false}" ); } );
		menu->addAction( QStringLiteral( "Settings..." ), this, [this] { present(); } );
		menu->addSeparator();
		menu->addAction( QStringLiteral( "Stop the dictionary daemon" ), this, [this] { client_->call( "shutdown" ); } );
		menu->addAction( QStringLiteral( "Quit" ), qApp, &QApplication::quit );
		tray_->setContextMenu( menu );

		connect( tray_, &QSystemTrayIcon::activated, this, [this]( QSystemTrayIcon::ActivationReason reason ) {
			if ( reason == QSystemTrayIcon::Trigger )
			{
				if ( isVisible() && isActiveWindow() )
				{
					hide();
				}
				else
				{
					present();
				}
			}
		} );
		tray_->show();
	}

	void MainWindow::closeEvent( QCloseEvent* event )
	{
		// With a tray icon the window only hides; the daemon keeps working either way.
		if ( tray_ != nullptr && tray_->isVisible() )
		{
			hide();
			event->ignore();
			return;
		}
		QMainWindow::closeEvent( event );
	}

	namespace
	{

		QString uptimeText( std::int64_t seconds )
		{
			if ( seconds < 60 )
			{
				return QStringLiteral( "%1 s" ).arg( seconds );
			}
			if ( seconds < 3600 )
			{
				return QStringLiteral( "%1 min" ).arg( seconds / 60 );
			}
			return QStringLiteral( "%1 h %2 min" ).arg( seconds / 3600 ).arg( ( seconds % 3600 ) / 60 );
		}

		// The daemon at a glance: which one, how long it has run, what it has done, and what went over the socket.
		QString statusLine( const json::Value& status, std::uint64_t sent, std::uint64_t received )
		{
			const auto lookups = status["lookups"].asInt();
			QString    line    = QStringLiteral( "Connected to the daemon %1 · pid %2 · up %3 · %4 dictionaries · %5 lookups" )
			                             .arg( qs( status["version"].asString() ) )
			                             .arg( status["pid"].asInt() )
			                             .arg( uptimeText( status["uptime"].asInt() ) )
			                             .arg( status["dictionaries"].asInt() )
			                             .arg( lookups );
			if ( lookups > 0 )
			{
				line += QStringLiteral( " (%1 µs each)" ).arg( status["average_lookup_us"].asDouble(), 0, 'f', 1 );
			}
			return line + QStringLiteral( " · %1 sent, %2 received" ).arg( formatBytes( sent ), formatBytes( received ) );
		}

	} // namespace

	void MainWindow::updateStatus( const json::Value& status )
	{
		connection_->setText( statusLine( status, client_->bytesSent(), client_->bytesReceived() ) );
		client_->setDaemonPid( status["pid"].asInt() );
		// After an update the daemon still running can be the older version: this one's replaces it, once.
		if ( !replaced_daemon_ && olderVersion( qs( status["version"].asString() ), qs( version ) ) )
		{
			replaced_daemon_ = true;
			client_->startDaemon( true );
		}
		const bool paused = status["paused"].asBool();
		if ( pause_action_ != nullptr )
		{
			pause_action_->setChecked( paused );
		}
		if ( tray_ != nullptr )
		{
			tray_->setToolTip( paused ? QStringLiteral( "Lexiglance (paused)" ) : QStringLiteral( "Lexiglance" ) );
		}
		if ( palette_from_daemon_ && status["dark_theme"].isBool() && status["dark_theme"].asBool() != dark_ )
		{
			applyPalette( status["dark_theme"].asBool() );
		}
		if ( auto* overview = dynamic_cast<OverviewPage*>( page_list_.front() ) )
		{
			overview->setStatus( status );
		}
	}

	void MainWindow::applyPalette( bool dark )
	{
		dark_ = dark;
		// The desktop's own (colourful) icons; the bundled Breeze icons, light or dark with the palette, fill in what its
		// theme lacks, and stand in on desktops without one.
		static const QString desktop_theme = QIcon::themeName();
		if ( !QIcon::themeSearchPaths().contains( QStringLiteral( ":/icons" ) ) )
		{
			QIcon::setThemeSearchPaths( QStringList( QStringLiteral( ":/icons" ) ) + QIcon::themeSearchPaths() );
		}
		const QString bundled = dark ? QStringLiteral( "lexiglance-dark" ) : QStringLiteral( "lexiglance" );
		if ( desktop_theme.isEmpty() || desktop_theme == QStringLiteral( "hicolor" ) )
		{
			QIcon::setThemeName( bundled );
		}
		else
		{
			QIcon::setThemeName( desktop_theme );
			QIcon::setFallbackThemeName( bundled );
		}
		QPalette palette;
		if ( dark )
		{
			const QColor window( 37, 38, 41 );
			const QColor base( 28, 29, 32 );
			const QColor text( 221, 222, 225 );
			const QColor button( 49, 50, 54 );
			palette.setColor( QPalette::Window, window );
			palette.setColor( QPalette::WindowText, text );
			palette.setColor( QPalette::Base, base );
			palette.setColor( QPalette::AlternateBase, window );
			palette.setColor( QPalette::ToolTipBase, button );
			palette.setColor( QPalette::ToolTipText, text );
			palette.setColor( QPalette::PlaceholderText, QColor( 128, 130, 136 ) );
			palette.setColor( QPalette::Text, text );
			palette.setColor( QPalette::Button, button );
			palette.setColor( QPalette::ButtonText, text );
			palette.setColor( QPalette::BrightText, QColor( 255, 90, 90 ) );
			palette.setColor( QPalette::Link, QColor( 110, 170, 255 ) );
			palette.setColor( QPalette::Highlight, QColor( 53, 120, 210 ) );
			palette.setColor( QPalette::HighlightedText, Qt::white );
			palette.setColor( QPalette::Mid, QColor( 60, 62, 66 ) );
			palette.setColor( QPalette::Dark, QColor( 24, 25, 27 ) );
			palette.setColor( QPalette::Disabled, QPalette::Text, QColor( 120, 122, 128 ) );
			palette.setColor( QPalette::Disabled, QPalette::ButtonText, QColor( 120, 122, 128 ) );
			palette.setColor( QPalette::Disabled, QPalette::WindowText, QColor( 120, 122, 128 ) );
		}
		else
		{
			palette = QApplication::style()->standardPalette();
		}
		QApplication::setPalette( palette );
		qApp->setStyleSheet( applicationStyle( dark ) );
	}

} // namespace lexiglance::gui
