#ifndef LEXIGLANCE_GUI_HELPOVERLAY_H
#define LEXIGLANCE_GUI_HELPOVERLAY_H

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <functional>

namespace lexiglance::gui
{

	// What can be done and where, on a rounded card over the window, which dims behind it. The pages it names are links
	// to them. Escape, a click beside the card or its button closes it. Dev builds can open the first-run setup again.
	class HelpOverlay final : public QWidget
	{
	public:
		HelpOverlay( std::function<void( const QString& )> show_page, std::function<void()> simulate_setup, QWidget* parent );

		// Covers the parent; `trigger` is the trigger's keys as shown to the user, `first` the first start's showing.
		void open( const QString& trigger, bool first );

		bool eventFilter( QObject* watched, QEvent* event ) override;

	protected:
		void paintEvent( QPaintEvent* event ) override;
		void mousePressEvent( QMouseEvent* event ) override;
		void keyPressEvent( QKeyEvent* event ) override;

	private:
		void place();

		std::function<void( const QString& )> show_page_;
		std::function<void()>                 simulate_setup_;
		QFrame*                               card_;
		QLabel*                               text_;
		QPushButton*                          close_;
		QPushButton*                          simulate_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_HELPOVERLAY_H
