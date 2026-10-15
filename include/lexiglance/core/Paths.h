#ifndef LEXIGLANCE_CORE_PATHS_H
#define LEXIGLANCE_CORE_PATHS_H

#include <lexiglance/core/Error.h>

#include <filesystem>
#include <string>

// Per-user locations following each platform's conventions (XDG on Linux). Setting LEXIGLANCE_HOME relocates
// everything below a single directory, which tests and portable installs rely on.
namespace lexiglance::paths
{

	[[nodiscard]] std::filesystem::path configDir();
	[[nodiscard]] std::filesystem::path dataDir();
	[[nodiscard]] std::filesystem::path cacheDir();
	[[nodiscard]] std::filesystem::path stateDir();
	[[nodiscard]] std::filesystem::path runtimeDir();

	[[nodiscard]] std::filesystem::path configFile();
	[[nodiscard]] std::filesystem::path dictionariesDir();
	[[nodiscard]] std::filesystem::path ocrDir();
	// Offline translation models, a directory for each.
	[[nodiscard]] std::filesystem::path translationDir();

	// Unix domain socket path, or a named pipe name on Windows.
	[[nodiscard]] std::string ipcEndpoint();

	// Where the AppImage goes in `home`: the folder of the user's own AppImages (Applications, applications, AppImages,
	// in whatever case; of several, the one with the most besides Lexiglance's files), else Applications.
	[[nodiscard]] std::filesystem::path appImageFolder( const std::filesystem::path& home );

	// Where the AppImage at `appimage` belongs instead: in appImageFolder() when it is alone in another folder of those
	// names (as an earlier update made ~/Applications beside the user's ~/applications); empty when it stays.
	[[nodiscard]] std::filesystem::path appImageDestination( const std::filesystem::path& appimage, const std::filesystem::path& home );

	Result<> ensureDirectory( const std::filesystem::path& dir, bool owner_only = false );

} // namespace lexiglance::paths

#endif // LEXIGLANCE_CORE_PATHS_H
