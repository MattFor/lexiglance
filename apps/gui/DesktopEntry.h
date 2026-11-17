#ifndef LEXIGLANCE_GUI_DESKTOPENTRY_H
#define LEXIGLANCE_GUI_DESKTOPENTRY_H

#include <QString>

// Where Lexiglance is started from outside the program: the desktop's applications menu (an XDG desktop entry of the
// user's, a Start menu shortcut on Windows) and the login (XDG autostart entries, the Run key on Windows, a LaunchAgent on
// macOS). An installed package brings its own menu entry; one started from a build tree or an AppImage adds one here.
//
// They all start one copy of Lexiglance, the installed one. A release copy (an AppImage, a package, an installed prefix,
// the Windows setup's or portable folder) becomes it by starting; a build tree only when .github/scripts/dev-install puts
// it there (lexiglance --claim-entries), so a build tried out now and then moves nothing.
namespace lexiglance::gui::desktop
{

	// Whether the menu shows Lexiglance: any entry that starts it (or that it wrote, or named like the package's), whatever
	// it is called and in whichever XDG data directory, that is not hidden.
	[[nodiscard]] bool menuEntryShown();

	// Shows Lexiglance in the menu (by undoing how it was taken out; a new entry only when that brings none back), or takes
	// it out (the entry it wrote goes, any other is hidden, the user's own in place). False (and `error`) on failure.
	bool setMenuEntry( bool shown, QString* error = nullptr );

	// The daemon at login (on macOS the settings application in the menu bar, which starts it).
	[[nodiscard]] bool autostartEnabled();
	bool               setAutostart( bool enabled );

#ifndef Q_OS_MACOS
	// The settings application at login as well, in the tray: a second entry beside the daemon's.
	[[nodiscard]] bool trayAutostartEnabled();
	bool               setTrayAutostart( bool enabled );
#endif

	// This copy of Lexiglance, as the daemon's status names the one it runs from ("program"): the AppImage file, else the
	// daemon beside this program.
	[[nodiscard]] QString thisCopy();

	// Whether two copies (thisCopy(), a daemon's "program") are the same one.
	[[nodiscard]] bool sameCopy( const QString& a, const QString& b );

	// Whether this program runs in the tree it was built in, rather than from where it was installed.
	[[nodiscard]] bool inBuildTree();

	// Points the autostart entries there are, and the menu entry this program wrote (unless Lexiglance was taken out of
	// the menu), at this copy; any other menu entry only once the Lexiglance it starts is gone. Of an entry changed by hand
	// only the program changes. The first time, a menu entry is added unless there is one.
	void claim();

	// On start: a release copy claims the entries, and is the installed copy from then on (true); a build tree only mends
	// the ones that start nothing any more, and adds the menu entry the first time. A copy with a home of its own
	// (LEXIGLANCE_HOME, as tests use) leaves the desktop's entries alone. An AppImage alone in a folder that only it is
	// in, beside the user's own folder for AppImages, first moves there (and the entries follow it).
	bool maintain();

} // namespace lexiglance::gui::desktop

#endif // LEXIGLANCE_GUI_DESKTOPENTRY_H
