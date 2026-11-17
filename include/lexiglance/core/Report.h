#ifndef LEXIGLANCE_CORE_REPORT_H
#define LEXIGLANCE_CORE_REPORT_H

#include <lexiglance/config/Config.h>
#include <lexiglance/core/Error.h>
#include <lexiglance/core/Json.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Problem reports (docs/debugging.md): what goes into the report zip, with secrets taken out, and the GitHub issue form
// filled in with its summary. Nothing is sent anywhere: the zip is saved, and the person attaches it to the issue.
namespace lexiglance::report
{

	// A file of the report zip, and what the list shown before saving calls it.
	struct Part
	{
		std::string name;
		std::string title;
		std::string contents;
	};

	struct Input
	{
		// What happened, in the person's words.
		std::string what;
		// Who makes the report ("lexiglance 1.4.1 (stable build), Qt 6.8.3").
		std::string program;
		// What the daemon answered; null when it is not running.
		const json::Value* status       = nullptr;
		const json::Value* health       = nullptr;
		const json::Value* dictionaries = nullptr;
		// How much of each log goes in.
		std::size_t log_lines = 1500;
	};

	// This system: its name and version, processor, and on Linux the desktop and the session type (X11, Wayland).
	[[nodiscard]] std::string system();

	// An address without the secrets it may hold: a user name and password, the values of key, token and password
	// parameters.
	[[nodiscard]] std::string withoutSecrets( std::string_view url );

	// The settings as a report shows them: the AnkiConnect key and the secrets in addresses taken out.
	[[nodiscard]] std::string redactedConfig( config::Config config );

	// `text` with the home folder written as ~, so paths and log lines do not name the person.
	[[nodiscard]] std::string withoutHome( std::string text );

	// The last `count` lines of a text file, oldest first.
	[[nodiscard]] std::vector<std::string> tail( const std::filesystem::path& file, std::size_t count );

	// The key facts in a few lines: versions, system, how the screen is read, dictionaries, and the health report's
	// errors and warnings.
	[[nodiscard]] std::string summary( const Input& input );

	// The report's files: what happened with the summary, the daemon's status and health report, the settings, the
	// dictionaries (names and languages, not their contents) and the end of each log.
	[[nodiscard]] std::vector<Part> parts( const Input& input );

	// GitHub's new issue form for bug reports, filled in; under 7 KB, so every browser opens it.
	[[nodiscard]] std::string issueUrl( std::string_view what, std::string_view summary );

	[[nodiscard]] Result<> save( const std::filesystem::path& zip, std::span<const Part> parts );

	// "lexiglance-report-20261117-201430.zip".
	[[nodiscard]] std::string fileName( std::chrono::system_clock::time_point when );

} // namespace lexiglance::report

#endif // LEXIGLANCE_CORE_REPORT_H
