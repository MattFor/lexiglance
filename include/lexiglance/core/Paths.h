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

	Result<> ensureDirectory( const std::filesystem::path& dir, bool owner_only = false );

} // namespace lexiglance::paths

#endif // LEXIGLANCE_CORE_PATHS_H
