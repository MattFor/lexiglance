#ifndef LEXIGLANCE_GUI_ABOUTPAGE_H
#define LEXIGLANCE_GUI_ABOUTPAGE_H

#include "Common.h"

#include <QCheckBox>
#include <QTextBrowser>

namespace lexiglance::gui
{

	class AboutPage : public Page
	{
	public:
		explicit AboutPage( Context context, QWidget* parent = nullptr );

		void activated() override;
		void refresh() override;

	private:
		void render();

		QTextBrowser* text_;
		// Developer mode's switch, shown once it is on or the version was clicked seven times.
		QCheckBox* developer_;
		int        version_clicks_ = 0;
		// The daemon's version, backend and text capture, or why it is not known.
		QString daemon_;
		// Attributions the installed dictionaries ask for (HTML).
		QString dictionaries_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_ABOUTPAGE_H
