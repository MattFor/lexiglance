#ifndef LEXIGLANCE_GUI_OVERVIEWPAGE_H
#define LEXIGLANCE_GUI_OVERVIEWPAGE_H

#include "Common.h"

#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <vector>

namespace lexiglance::gui
{

	class OverviewPage : public Page
	{
	public:
		explicit OverviewPage( Context context, QWidget* parent = nullptr );

		void setStatus( const json::Value& status );
		void refresh() override;

	private:
		// Asks the daemon for its self-checks, or tells what can be told without it, and lists them. `interactive`: the
		// user clicked for it.
		void                                            checkHealth( bool interactive );
		void                                            showChecks( const std::vector<health::Check>& checks );
		[[nodiscard]] static std::vector<health::Check> localChecks();
		void                                            fix( const QString& action );
		void                                            restart();
		void                                            finishRestart( bool ok, const QString& message );
		void                                            setTrigger( bool held );

		QLabel*                    state_;
		QLabel*                    details_;
		QLabel*                    trigger_;
		QPushButton*               pause_;
		QPushButton*               restart_;
		QCheckBox*                 autostart_;
		QLineEdit*                 test_text_;
		QLabel*                    test_result_;
		QLabel*                    dictionaries_value_;
		QLabel*                    lookups_value_;
		QLabel*                    speed_value_;
		QLabel*                    capture_value_;
		QPushButton*               check_;
		QLabel*                    health_summary_;
		QCheckBox*                 show_passed_;
		QVBoxLayout*               health_rows_;
		QLabel*                    trigger_state_;
		QLabel*                    last_capture_;
		QTimer*                    restart_timer_;
		std::vector<health::Check> checks_;
		bool                       checking_        = false;
		bool                       restarting_      = false;
		qint64                     restart_from_    = 0;
		qint64                     restart_started_ = 0;
		bool                       paused_          = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_OVERVIEWPAGE_H
