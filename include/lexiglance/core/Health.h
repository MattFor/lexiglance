#ifndef LEXIGLANCE_CORE_HEALTH_H
#define LEXIGLANCE_CORE_HEALTH_H

#include <lexiglance/core/Json.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The daemon's self-checks: each says whether one part works, why not, and what fixes it. Shown by the settings
// application ("Check health") and by `lexiglancectl health`.
namespace lexiglance::health
{

	enum class Severity : std::uint8_t
	{
		Ok,
		Info,
		Warning,
		Error
	};

	struct Check
	{
		std::string id;
		std::string title;
		Severity    status = Severity::Ok;
		std::string detail;
		// The action that fixes it, understood by the settings application: "restart", "resume", "enable-ocr",
		// "enable-accessibility", "open-dictionaries", "open-scanning", "open-anki"; empty when there is none.
		std::string fix;
	};

	[[nodiscard]] std::string_view statusName( Severity status ) noexcept;

	[[nodiscard]] Severity parseStatus( std::string_view name ) noexcept;

	// {"checks": [{id, title, status, detail, fix}], "errors": n, "warnings": n}
	void write( json::Writer& out, const std::vector<Check>& checks );

	// The checks of a written report.
	[[nodiscard]] std::vector<Check> read( const json::Value& report );

} // namespace lexiglance::health

#endif // LEXIGLANCE_CORE_HEALTH_H
