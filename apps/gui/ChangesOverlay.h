#ifndef LEXIGLANCE_GUI_CHANGESOVERLAY_H
#define LEXIGLANCE_GUI_CHANGESOVERLAY_H

#include "CardOverlay.h"

#include <QLabel>
#include <QPushButton>

namespace lexiglance::gui
{

	// What changed in a version, from the changelog built into the program, on a rounded card over the window: shown
	// once after an update. Escape, a click beside the card or its button closes it.
	class ChangesOverlay final : public CardOverlay
	{
	public:
		explicit ChangesOverlay( QWidget* parent );

		// Covers the parent with the changes in `version`; the newest in the changelog when it has none of its own.
		void open( const QString& version );

	private:
		QLabel*      title_;
		QLabel*      text_;
		QPushButton* close_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_CHANGESOVERLAY_H
