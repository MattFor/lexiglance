#ifndef LEXIGLANCE_DAEMON_ELEVATION_H
#define LEXIGLANCE_DAEMON_ELEVATION_H

#include <lexiglance/core/Error.h>

#include <filesystem>
#include <span>
#include <string>

// Run as administrator (Windows only, Overview -> Startup). While a program that runs as administrator is in front
// (many games and their launchers do), Windows keeps its keys and clicks from programs that do not, so the trigger never
// reaches an unelevated daemon. Turned on, the daemon runs as administrator too: Windows asks once, when a scheduled
// task is set up that starts this daemon with the highest privileges. From then on a daemon started without them (at
// login, by the settings application) starts that task and makes way for the daemon it starts; nothing asks again.
//
// The task's user may start and delete it, but not change what it starts: that takes an administrator. Uninstalling
// deletes it, so it never outlives the program it starts.
namespace lexiglance::daemon::elevation
{

	// Whether this process runs as administrator (elevated).
	[[nodiscard]] bool elevated();

	// This user's account (its SID, "S-1-5-21-..."), which the task runs as.
	[[nodiscard]] std::string userSid();

	// The program this user's task starts; empty when there is no task.
	[[nodiscard]] Result<std::filesystem::path> taskProgram();

	// Sets up the task to start `program` (with --from-task), in place of one there may be. Needs administrator rights.
	Result<> install( const std::filesystem::path& program );

	// Deletes the task; there being none is fine too.
	Result<> remove();

	// Starts the task: its program, as administrator. Windows starts it a moment later.
	Result<> start();

	// Starts a program as the desktop (Explorer) runs, without administrator rights, for what an elevated daemon starts
	// that has no need of them. False when that cannot be done.
	bool startUnelevated( const std::filesystem::path& program, std::span<const std::string> arguments );

} // namespace lexiglance::daemon::elevation

#endif // LEXIGLANCE_DAEMON_ELEVATION_H
