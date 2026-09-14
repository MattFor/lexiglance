#ifndef LEXIGLANCE_LANGUAGE_DEFINED_H
#define LEXIGLANCE_LANGUAGE_DEFINED_H

#include <lexiglance/core/Error.h>
#include <lexiglance/language/Language.h>

#include <memory>
#include <vector>

namespace lexiglance::json
{
	class Value;
}

// Languages defined by their files (docs/languages.md).
namespace lexiglance::lang
{

	[[nodiscard]] Result<std::unique_ptr<Language>> defineLanguage( const json::Value& root );

	// The dictionaries a language file recommends.
	[[nodiscard]] std::vector<Recommendation> recommendationsOf( const json::Value& root );

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_DEFINED_H
