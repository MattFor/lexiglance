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

	// The translation model a language file names ("translation"), empty when it names none.
	[[nodiscard]] TranslationModel translationModelOf( const json::Value& root );

	// The letters a language file says only its language has ("distinctive_letters").
	[[nodiscard]] std::u32string distinctiveLettersOf( const json::Value& root );

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_DEFINED_H
