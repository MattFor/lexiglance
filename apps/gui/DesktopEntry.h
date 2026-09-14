#ifndef LEXIGLANCE_GUI_DESKTOPENTRY_H
#define LEXIGLANCE_GUI_DESKTOPENTRY_H

#include <QString>

// The settings application in the desktop's applications menu (an XDG desktop entry of the user's). An installed
// package brings its own entry; one started from a build tree or an AppImage adds one here.
namespace lexiglance::gui::desktop
{

	// Whether the menu shows Lexiglance: an entry of the user's, or an installed one the user has not hidden.
	[[nodiscard]] bool menuEntryShown();

	// Adds an entry that starts this program, or takes Lexiglance out of the menu. False (and `error`) on failure.
	bool setMenuEntry( bool shown, QString* error = nullptr );

	// On start: adds the entry the first time, and keeps an entry this program wrote pointing at where it now is.
	void maintainMenuEntry();

} // namespace lexiglance::gui::desktop

#endif // LEXIGLANCE_GUI_DESKTOPENTRY_H
