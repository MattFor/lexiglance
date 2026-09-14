#ifndef LEXIGLANCE_LANGUAGE_JA_FURIGANA_H
#define LEXIGLANCE_LANGUAGE_JA_FURIGANA_H

#include <lexiglance/language/Language.h>

#include <string_view>
#include <vector>

namespace lexiglance::lang::ja
{

	// Aligns a reading with an expression so furigana sits only above kanji: 食べ物/たべもの -> 食(た) べ 物(もの).
	[[nodiscard]] std::vector<RubySegment> distributeFurigana( std::string_view expression, std::string_view reading );

} // namespace lexiglance::lang::ja

#endif // LEXIGLANCE_LANGUAGE_JA_FURIGANA_H
