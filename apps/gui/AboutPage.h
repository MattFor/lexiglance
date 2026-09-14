#ifndef LEXIGLANCE_GUI_ABOUTPAGE_H
#define LEXIGLANCE_GUI_ABOUTPAGE_H

#include "Common.h"

#include <QTextBrowser>

namespace lexiglance::gui
{

	class AboutPage : public Page
	{
	public:
		explicit AboutPage( Context context, QWidget* parent = nullptr );

		void activated() override;

	private:
		void render();

		// The version, the daemon and the system as plain text, for bug reports.
		[[nodiscard]] QString systemInformation() const;

		QTextBrowser* text_;
		// The daemon's version, backend and text capture, or why it is not known.
		QString daemon_;
		// Attributions the installed dictionaries ask for (HTML).
		QString dictionaries_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_ABOUTPAGE_H
