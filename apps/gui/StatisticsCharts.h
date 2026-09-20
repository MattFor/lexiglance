#ifndef LEXIGLANCE_GUI_STATISTICSCHARTS_H
#define LEXIGLANCE_GUI_STATISTICSCHARTS_H

#include <QColor>
#include <QDate>
#include <QString>
#include <QWidget>

#include <array>
#include <cstdint>
#include <vector>

namespace lexiglance::gui
{

	// What the tally counts each day, in the order the daemon writes them.
	enum class Figure : std::uint8_t
	{
		Lookups,
		Found,
		Characters,
		Popups,
		Audio,
		Anki,
		Translations,
		TranslatedCharacters,
	};
	constexpr std::size_t figure_count = 8;

	// How a figure is called on the page ("Characters read"), and its name in the daemon's tally ("characters").
	[[nodiscard]] QString     figureName( Figure figure );
	[[nodiscard]] const char* figureKey( Figure figure );

	// A day, or a week of them when the range is long, with every figure counted in it.
	struct Period
	{
		QDate                                  first;
		QDate                                  last;
		std::array<std::int64_t, figure_count> values{};

		[[nodiscard]] std::int64_t operator[]( Figure figure ) const
		{
			return values[static_cast<std::size_t>( figure )];
		}
	};

	// One figure over time, as bars or a line. The pointer over a day shows a card with all that day's figures.
	class ActivityChart : public QWidget
	{
	public:
		enum class Style : std::uint8_t
		{
			Bars,
			Line,
		};

		explicit ActivityChart( QWidget* parent = nullptr );

		void setPeriods( std::vector<Period> periods, Figure figure, Style style );

		[[nodiscard]] QSize sizeHint() const override;
		[[nodiscard]] QSize minimumSizeHint() const override;

	protected:
		void paintEvent( QPaintEvent* event ) override;
		void mouseMoveEvent( QMouseEvent* event ) override;
		void leaveEvent( QEvent* event ) override;

	private:
		[[nodiscard]] QRectF plot() const;
		[[nodiscard]] int    periodAt( double x ) const;
		void                 paintCard( QPainter& painter, const Period& period, double x ) const;

		std::vector<Period> periods_;
		Figure              figure_ = Figure::Lookups;
		Style               style_  = Style::Bars;
		// The period under the pointer, or -1.
		int hover_ = -1;
	};

	// Counts side by side as bars, the largest first: what it is, how often, and its share of all of them on hover.
	class BarList : public QWidget
	{
	public:
		struct Row
		{
			QString      label;
			std::int64_t count = 0;
		};

		explicit BarList( QWidget* parent = nullptr );

		void setRows( std::vector<Row> rows, const QColor& colour, const QString& empty );

		[[nodiscard]] QSize sizeHint() const override;

	protected:
		void paintEvent( QPaintEvent* event ) override;
		void mouseMoveEvent( QMouseEvent* event ) override;
		void leaveEvent( QEvent* event ) override;
		bool event( QEvent* event ) override;

	private:
		[[nodiscard]] int rowHeight() const;
		[[nodiscard]] int rowAt( int y ) const;

		std::vector<Row> rows_;
		QColor           colour_;
		QString          empty_;
		int              hover_ = -1;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_STATISTICSCHARTS_H
