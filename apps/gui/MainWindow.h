#ifndef LEXIGLANCE_GUI_MAINWINDOW_H
#define LEXIGLANCE_GUI_MAINWINDOW_H

#include "Common.h"
#include "DaemonClient.h"
#include "ChangesOverlay.h"
#include "HelpOverlay.h"
#include "Settings.h"
#include "SetupWizard.h"

#include <QAction>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QSystemTrayIcon>

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

		// What changed in `version`, over the window: once after an update, or from Help in dev builds.
		void showChanges( const QString& version );

		// First-run setup wizard (also from Help). `lock_seconds` 0 when simulating from Help.
		// `required`: first-run — the wizard cannot be skipped.
		void showSetup( int lock_seconds = 5, bool reinstall = false, bool required = false );

		// The setup on the first start (installs from before it, which saw the short help, skip it).
		void welcome();

		// This is the installed copy (see DesktopEntry.h): a daemon of another copy is replaced by this one's.
		void replaceOtherDaemon();

	protected:
		void closeEvent( QCloseEvent* event ) override;

	private:
		void addPage( Page* page, const QString& title, const QString& icon );
		void setupTray();
		void updateStatus( const json::Value& status );
		void applyPalette( bool dark );

		DaemonClient*             client_;
		std::unique_ptr<Settings> settings_;
		QListWidget*              navigation_;
		QStackedWidget*           pages_;
		std::vector<Page*>        page_list_;
		QSystemTrayIcon*          tray_         = nullptr;
		int                       tray_tries_   = 0;
		QAction*                  pause_action_ = nullptr;
		HelpOverlay*              help_         = nullptr;
		ChangesOverlay*           changes_      = nullptr;
		SetupWizard*              setup_        = nullptr;
		QLabel*                   connection_;
		QLabel*                   summary_;
		QPushButton*              start_button_;
		bool                      palette_from_daemon_ = true;
		bool                      dark_                = false;
		bool                      tried_start_         = false;
		bool                      replaced_daemon_     = false;
		bool                      replace_other_       = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_MAINWINDOW_H
