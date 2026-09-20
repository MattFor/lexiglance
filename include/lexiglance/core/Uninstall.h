#ifndef LEXIGLANCE_CORE_UNINSTALL_H
#define LEXIGLANCE_CORE_UNINSTALL_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Taking Lexiglance off this computer, for the settings application's Uninstall and `lexiglancectl uninstall`. What
// goes depends on how this copy was installed; the Windows setup has an uninstaller of its own, which both hand over to.
namespace lexiglance::uninstall
{

	enum class Kind : std::uint8_t
	{
		WindowsSetup,    // installed with the Windows setup: its Uninstall.exe removes it
		WindowsPortable, // the portable .zip, unpacked into a folder
		AppImage,        // the AppImage: one file
		Deb,             // the .deb package, removed with apt-get
		Rpm,             // the .rpm package, removed with rpm
		Prefix,          // installed into a prefix: cmake --install, or the .tar.gz unpacked
		BuildTree,       // runs where it was built; the build itself stays
	};

	// Where things are: the running program, and the directories of the user's that the desktop and Lexiglance read.
	struct Places
	{
		std::filesystem::path executable;
		// $APPIMAGE, when running from one.
		std::filesystem::path appimage;
		std::filesystem::path home;
		// $XDG_CONFIG_HOME and $XDG_DATA_HOME (~/.config, ~/.local/share).
		std::filesystem::path config_home;
		std::filesystem::path data_home;
		// Lexiglance's settings, dictionaries and models, cache and logs (paths::configDir() and the others).
		std::vector<std::filesystem::path> data;

		// This process's.
		[[nodiscard]] static Places current();
	};

	struct Plan
	{
		Kind kind = Kind::Prefix;
		// The Windows uninstaller, the AppImage, the portable folder, the prefix or the build tree.
		std::filesystem::path where;
		// Lexiglance's own files: the programs and what came with them in the prefix, the AppImage, the portable folder's.
		std::vector<std::filesystem::path> programs;
		// The command that removes the package (run as root), in place of `programs`.
		std::vector<std::string> package;
		// The user's autostart entries, menu entry and icon, and service definitions copied from the documentation.
		std::vector<std::filesystem::path> integration;
		// Settings, dictionaries, downloaded models, logs and cache; empty when they are kept.
		std::vector<std::filesystem::path> data;
	};

	// What taking this copy off involves; only paths that exist are listed.
	[[nodiscard]] Plan plan( const Places& places, bool keep_data );

	// What a plan does, a line for each part, for the user to confirm.
	[[nodiscard]] std::vector<std::string> describe( const Plan& plan );

	// Whether deleting `path` takes root: its folder is not this user's to change.
	[[nodiscard]] bool needsRoot( const std::filesystem::path& path );

	// Stops the services that would start the daemon again once it ends (systemd, runit, dinit and OpenRC user
	// services), then the daemon and the settings application themselves: asked to end, killed after `grace`.
	void stopPrograms( const Places& places, std::chrono::milliseconds grace );

	// Deletes the paths, folders with everything in them (a link, not what it points at), then the Lexiglance folders
	// they leave empty. What could not be deleted comes back, a line for each with why.
	[[nodiscard]] std::vector<std::string> remove( std::span<const std::filesystem::path> paths );

} // namespace lexiglance::uninstall

#endif // LEXIGLANCE_CORE_UNINSTALL_H
