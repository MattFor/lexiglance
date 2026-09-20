#ifndef LEXIGLANCE_GUI_STATISTICSPAGE_H
#define LEXIGLANCE_GUI_STATISTICSPAGE_H

#include "Common.h"
#include "StatisticsCharts.h"

#include <lexiglance/core/Json.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QLabel>
#include <QTimer>

#include <array>
#include <map>
#include <vector>

namespace lexiglance::gui
{

	// What Lexiglance has done so far: how much was looked up and translated, in which languages, through what, and
	// which words keep coming back, over the last days or all the time, with a chart whose days show their own figures
	// under the pointer. The daemon keeps the tally between runs; this page only shows it.
	class StatisticsPage : public Page
	{
	public:
		explicit StatisticsPage( Context context, QWidget* parent = nullptr );

		void activated() override;
		void refresh() override;

	private:
		// The tally as the daemon sent it, kept so the range and the chart can change without asking again.
		struct Tally
		{
			bool                                                    enabled              = true;
			bool                                                    translations_enabled = true;
			QDate                                                   first_day;
			std::int64_t                                            sessions       = 0;
			std::int64_t                                            distinct_words = 0;
			double                                                  lookup_us      = 0.0;
			double                                                  translation_ms = 0.0;
			std::array<std::int64_t, figure_count>                  totals{};
			std::map<QDate, std::array<std::int64_t, figure_count>> days;
			std::vector<BarList::Row>                               words;
			std::vector<BarList::Row>                               languages;
			std::vector<BarList::Row>                               translated;
			std::vector<BarList::Row>                               sources;
		};

		void update();
		void read( const json::Value& stats );
		// The figures, the chart and the lists for the chosen range, figure and style.
		void show();
		// The days of the chosen range (weeks when it is long), oldest first.
		[[nodiscard]] std::vector<Period> periods() const;
		void                              remember() const;

		std::array<QLabel*, 12> values_{};
		std::array<QLabel*, 12> captions_{};
		QButtonGroup*           range_;
		QComboBox*              figure_;
		QButtonGroup*           style_;
		ActivityChart*          chart_;
		BarList*                words_;
		BarList*                languages_;
		BarList*                translated_;
		BarList*                sources_;
		QCheckBox*              counting_;
		QCheckBox*              counting_translations_;
		QLabel*                 note_;
		QTimer*                 poll_;
		Tally                   tally_;
		bool                    loaded_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_STATISTICSPAGE_H
