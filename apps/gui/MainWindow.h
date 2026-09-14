#ifndef LEXIGLANCE_GUI_MAINWINDOW_H
#define LEXIGLANCE_GUI_MAINWINDOW_H

#include "Common.h"
#include "DaemonClient.h"
#include "Settings.h"

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
		QAction*                  pause_action_ = nullptr;
		QLabel*                   connection_;
		QLabel*                   summary_;
		QPushButton*              start_button_;
		bool                      palette_from_daemon_ = true;
		bool                      dark_                = false;
		bool                      tried_start_         = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_MAINWINDOW_H
