#ifndef LEXIGLANCE_GUI_OVERVIEWPAGE_H
#define LEXIGLANCE_GUI_OVERVIEWPAGE_H

#include "Common.h"
#include "Downloader.h"
#include "TranslationInstall.h"
#include "UninstallOverlay.h"

#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <vector>

namespace lexiglance::gui
{

	class OverviewPage : public Page
	{
	public:
		explicit OverviewPage( Context context, QWidget* parent = nullptr );

		void setStatus( const json::Value& status );
		void refresh() override;

	protected:
		void showEvent( QShowEvent* event ) override;

	private:
		// Asks the daemon for its self-checks, or tells what can be told without it, and lists them. `interactive`: the
		// user clicked for it.
		// What the daemon reports while the page is open.
		void                                            onDaemonEvent( std::string_view name, const json::Value& params );
		void                                            checkHealth( bool interactive );
		void                                            showChecks( const std::vector<health::Check>& checks );
		[[nodiscard]] static std::vector<health::Check> localChecks();
		void                                            fix( const QString& action );
		// Every problem that can be put right without a decision, in one go.
		void fixAll();
		// Windows: Microsoft's runtime downloaded, installed, and the daemon started again on the new one.
		void installRuntime();
		// Downloads the translation models of the languages turned on, then runs `then`.
		void downloadTranslation( std::function<void()> then );
		void restart();
		void finishRestart( bool ok, const QString& message );
		void setTrigger( bool held );

		QLabel*       state_;
		QLabel*       details_;
		QLabel*       trigger_;
		QPushButton*  pause_;
		QPushButton*  restart_;
		QCheckBox*    autostart_;
		QCheckBox*    autostart_tray_;
		QLineEdit*    test_text_;
		QLabel*       test_result_;
		QLabel*       dictionaries_value_;
		QLabel*       lookups_value_;
		QLabel*       speed_value_;
		QLabel*       capture_value_;
		QPushButton*  check_;
		QPushButton*  fix_all_;
		QLabel*       health_summary_;
		QProgressBar* health_progress_;
		// Created for the first translation download the health check asks for.
		translation_install::Installer* translation_ = nullptr;
		// The Uninstall card, made when it is first asked for.
		UninstallOverlay* uninstall_ = nullptr;
		QCheckBox*        show_passed_;
		QVBoxLayout*      health_rows_;
		QLabel*           trigger_state_;
		QLabel*           last_capture_;
		// The Health box and the area it scrolls in, so the capture tile can point the user at it.
		QGroupBox*                 health_box_;
		QScrollArea*               scroll_;
		Downloader*                downloader_;
		QTimer*                    restart_timer_;
		QTimer*                    health_timer_;
		std::vector<health::Check> checks_;
		bool                       checking_        = false;
		bool                       fixing_          = false;
		bool                       restarting_      = false;
		qint64                     restart_from_    = 0;
		qint64                     restart_started_ = 0;
		bool                       paused_          = false;
		bool                       health_stale_    = false;
		// A lookup seen while this window was open; until then the health report says when the last one was.
		bool saw_lookup_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_OVERVIEWPAGE_H
