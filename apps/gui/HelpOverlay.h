#ifndef LEXIGLANCE_GUI_HELPOVERLAY_H
#define LEXIGLANCE_GUI_HELPOVERLAY_H

#include "CardOverlay.h"

#include <QLabel>
#include <QPushButton>

#include <functional>

namespace lexiglance::gui
{

	// What can be done and where, on a rounded card over the window, which dims behind it. The pages it names are links
	// to them. Escape, a click beside the card or its button closes it. Dev builds can open the first-run setup again.
	class HelpOverlay final : public CardOverlay
	{
	public:
		HelpOverlay( std::function<void( const QString& )> show_page, std::function<void()> simulate_setup, QWidget* parent );

		// Covers the parent; `trigger` is the trigger's keys as shown to the user, `sentence` the sentence key (empty when
		// there is none), `first` the first start's showing.
		void open( const QString& trigger, const QString& sentence, bool first );

	private:
		std::function<void( const QString& )> show_page_;
		std::function<void()>                 simulate_setup_;
		QLabel*                               text_;
		QPushButton*                          close_;
		QPushButton*                          simulate_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_HELPOVERLAY_H
