#ifndef LEXIGLANCE_GUI_MAINWINDOW_H
#define LEXIGLANCE_GUI_MAINWINDOW_H

#include "Common.h"
#include "DaemonClient.h"
#include "ChangesOverlay.h"
#include "DeveloperPage.h"
#include "DictionariesPage.h"
#include "HelpOverlay.h"
#include "ReportOverlay.h"
#include "SearchPage.h"
#include "Settings.h"
#include "SetupWizard.h"

#include <QAction>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>
#include <vector>

namespace lexiglance::gui
{

	class MainWindow : public QMainWindow
	{
	public:
		MainWindow();

		void present();

		void showPage( const QString& name );

		void search( const QString& text );

		// What can be done and where, over the window; `first` on the first start.
		void showHelp( bool first = false );

		// What changed in `version`, over the window (since `since`, every version after it, the newest first): once after
		// an update, or from Help.
		void showChanges( const QString& version, const QString& since = {} );

		// First-run setup wizard (also from Help). `lock_seconds` 0 when simulating from Help.
		// `required`: first-run — the wizard cannot be skipped.
		void showSetup( int lock_seconds = 5, bool reinstall = false, bool required = false );

		// The setup on the first start (installs from before it, which saw the short help, skip it).
		void welcome();

		// Report a problem, over the window.
		void showReport();

		// This is the installed copy (see DesktopEntry.h): a daemon of another copy is replaced by this one's.
		void replaceOtherDaemon();

	protected:
		void closeEvent( QCloseEvent* event ) override;
		// Dictionaries dropped anywhere on the window are imported.
		void dragEnterEvent( QDragEnterEvent* event ) override;
		void dropEvent( QDropEvent* event ) override;

	private:
		void addPage( Page* page, const QString& title, const QString& icon );
		void setupTray();
		// The tray at a glance: paused or not, the trigger, the window of the latest lookup.
		void updateTray();
		// The words of the latest lookups, for the tray's menu and the Search page.
		void refreshRecent();
		void updateStatus( const json::Value& status );
		void applyPalette( bool dark );
		// The Developer page in the sidebar while developer mode is on; turned on first when it was asked for.
		void updateDeveloper();

		DaemonClient*             client_;
		std::unique_ptr<Settings> settings_;
		QListWidget*              navigation_;
		QStackedWidget*           pages_;
		std::vector<Page*>        page_list_;
		QSystemTrayIcon*          tray_          = nullptr;
		int                       tray_tries_    = 0;
		QAction*                  pause_action_  = nullptr;
		QAction*                  ignore_action_ = nullptr;
		QMenu*                    recent_menu_   = nullptr;
		QTimer*                   recent_timer_  = nullptr;
		QIcon                     tray_icon_;
		bool                      tray_faded_ = false;
		// The window class of the latest lookup on screen, as Scanning -> Ignored windows matches it.
		QString           last_window_;
		SearchPage*       search_page_       = nullptr;
		DictionariesPage* dictionaries_page_ = nullptr;
		HelpOverlay*      help_              = nullptr;
		ChangesOverlay*   changes_           = nullptr;
		SetupWizard*      setup_             = nullptr;
		ReportOverlay*    report_            = nullptr;
		DeveloperPage*    developer_         = nullptr;
		// lexiglance --developer, or the page asked for by name: developer mode goes on once the settings are loaded.
		bool         developer_wanted_ = false;
		QLabel*      connection_;
		QLabel*      summary_;
		QPushButton* start_button_;
		bool         palette_from_daemon_ = true;
		bool         dark_                = false;
		bool         tried_start_         = false;
		bool         replaced_daemon_     = false;
		bool         replace_other_       = false;
		bool         restarting_          = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_MAINWINDOW_H
