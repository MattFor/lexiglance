#ifndef LEXIGLANCE_GUI_STATISTICSPAGE_H
#define LEXIGLANCE_GUI_STATISTICSPAGE_H

#include "Common.h"

#include <lexiglance/core/Json.h>

#include <QCheckBox>
#include <QLabel>
#include <QTextBrowser>
#include <QTimer>

#include <array>

namespace lexiglance::gui
{

	// What Lexiglance has done so far: how much was looked up, in which languages, through what, and which words keep
	// coming back. The daemon keeps the tally between runs; this page only shows it.
	class StatisticsPage : public Page
	{
	public:
		explicit StatisticsPage( Context context, QWidget* parent = nullptr );

		void activated() override;
		void refresh() override;

	private:
		void update();
		void show( const json::Value& stats );

		// The figures across the top, each with its caption.
		std::array<QLabel*, 8> figures_{};
		QTextBrowser*          detail_;
		QCheckBox*             counting_;
		QLabel*                note_;
		QTimer*                poll_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_STATISTICSPAGE_H
