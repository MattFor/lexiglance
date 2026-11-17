#ifndef LEXIGLANCE_GUI_CHANGESOVERLAY_H
#define LEXIGLANCE_GUI_CHANGESOVERLAY_H

#include "CardOverlay.h"

#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>

namespace lexiglance::gui
{

	// What changed, from the changelog built into the program, on a rounded card over the window: shown once after an
	// update, for every version since the one shown last, newest first, scrolling when it is long. Escape, Enter, a click
	// beside the card or its button closes it.
	class ChangesOverlay final : public CardOverlay
	{
	public:
		explicit ChangesOverlay( QWidget* parent );

		// Covers the parent with the changes in the versions after `since` up to `version`. Without `since` (or with
		// none of those in the changelog), those in `version`; the newest in the changelog when it has none of its own.
		void open( const QString& version, const QString& since = {} );

	private:
		QLabel*       title_;
		QTextBrowser* text_;
		QPushButton*  close_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_CHANGESOVERLAY_H
