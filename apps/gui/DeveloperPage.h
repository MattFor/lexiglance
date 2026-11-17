#ifndef LEXIGLANCE_GUI_DEVELOPERPAGE_H
#define LEXIGLANCE_GUI_DEVELOPERPAGE_H

#include "Common.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Report.h>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	// Developer mode's page (docs/debugging.md), shown once it is turned on (About: click the version seven times, or
	// lexiglance --developer): the pipeline trace live, the capture inspector, scans, lookups and states simulated through
	// the daemon's own pipeline (never input sent to the system), the daemon's log as it is written, an IPC console and the
	// diagnostics bundle. The daemon sends the trace and the log only while the page is open.
	class DeveloperPage : public Page
	{
	public:
		struct Actions
		{
			std::function<void()>                 show_setup;
			std::function<void( const QString& )> show_changes;
		};

		DeveloperPage( Context context, Actions actions, QWidget* parent = nullptr );

		void refresh() override;

		// The trace gathered while the page was open, for a problem report; none before any.
		[[nodiscard]] std::vector<report::Part> reportParts() const;

	protected:
		void showEvent( QShowEvent* event ) override;
		void hideEvent( QHideEvent* event ) override;

	private:
		QWidget* tracePage();
		QWidget* capturePage();
		QWidget* simulatePage();
		QWidget* logPage();
		QWidget* consolePage();

		void subscribe( bool on );
		void onEvent( std::string_view name, const json::Value& params );
		void addTrace( const json::Value& event );
		void addLog( const QString& level, const QString& line );
		void showLog();
		// In three seconds, where the pointer is then (for pointing at something in another window): into `x` and `y`.
		void pickPoint( QPushButton* button, QSpinBox* x, QSpinBox* y, std::function<void()> then );
		void capture();
		void showCapture( const json::Value& result );
		// The daemon's answer to a simulation, in the status line.
		void ask( std::string_view method, const std::string& params, const QString& done );
		void simulateProblems();
		void exportBundle();

		Actions         actions_;
		QStackedWidget* pages_;
		QLabel*         note_;
		// The daemon sends the trace and the log lines (it refuses while developer mode is off there).
		bool subscribed_ = false;

		QTreeWidget*                             trace_;
		QLabel*                                  percentiles_;
		QCheckBox*                               trace_paused_;
		std::map<std::int64_t, QTreeWidgetItem*> rows_;
		std::array<std::pair<double, double>, 5> latest_{};
		std::array<bool, 5>                      timed_{};
		std::deque<std::string>                  trace_lines_;

		QSpinBox*       capture_x_;
		QSpinBox*       capture_y_;
		QCheckBox*      capture_picture_;
		QPushButton*    capture_button_;
		QPushButton*    capture_later_;
		QPlainTextEdit* capture_text_;
		QLabel*         capture_image_;

		QSpinBox*               scan_x_;
		QSpinBox*               scan_y_;
		QCheckBox*              scan_sentence_;
		QLineEdit*              lookup_text_;
		std::vector<QCheckBox*> problems_;
		QLineEdit*              changes_version_;
		QLineEdit*              update_version_;

		QComboBox*                          log_filter_;
		QComboBox*                          log_level_;
		QPlainTextEdit*                     log_;
		std::deque<std::pair<int, QString>> log_lines_;

		QLineEdit*      console_method_;
		QPlainTextEdit* console_params_;
		QCheckBox*      console_events_;
		QPlainTextEdit* console_output_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_DEVELOPERPAGE_H
