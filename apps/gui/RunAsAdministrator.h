#ifndef LEXIGLANCE_GUI_RUNASADMINISTRATOR_H
#define LEXIGLANCE_GUI_RUNASADMINISTRATOR_H

#include <QString>

#include <functional>

class QWidget;

// Run as administrator (Windows only, Overview -> Startup): the daemon runs with administrator rights, so that its
// trigger works over programs that run with them too (see apps/daemon/Elevation.h). The daemon program sets up and
// removes the scheduled task that starts it so; this runs it for the settings application, as administrator for
// setting it up (Windows asks). Nothing here does anything off Windows.
namespace lexiglance::gui::administrator
{

	// Whether the option exists on this system.
	[[nodiscard]] bool available();

	// Sets it up (`on`) or removes it; `done` gets an error message, empty once done. Setting it up shows Windows'
	// prompt for administrator rights; removing it asks only if it cannot be removed without them. `owner` keeps the
	// work alive: `done` does not come once it is gone.
	void set( QWidget* owner, bool on, const std::function<void( const QString& )>& done );

	// Removes it at once, as uninstalling does, waiting a few seconds at most. Best effort.
	void removeNow();

} // namespace lexiglance::gui::administrator

#endif // LEXIGLANCE_GUI_RUNASADMINISTRATOR_H
