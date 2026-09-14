#ifndef LEXIGLANCE_LANGUAGE_ALPHABETIC_H
#define LEXIGLANCE_LANGUAGE_ALPHABETIC_H

#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>

namespace lexiglance::lang
{

	// Base for languages written in an alphabet with upper and lower case (Latin, Greek, Cyrillic). Words are looked up
	// as written, in lower case (the first word of a sentence) and capitalised (a name in capitals), without stress marks,
	// and with the language's folds() (Russian ё as е). A derived class adds its deinflection rules to `deinflector_` in
	// its constructor and then calls `deinflector_.finalize()`.
	class AlphabeticLanguage : public Language
	{
	public:
		// Letters, stress marks, and the hyphens and apostrophes inside words.
		[[nodiscard]] bool isLookupCharacter( char32_t c ) const noexcept override;

		void variants( std::u32string_view source, std::vector<TextVariant>& out ) const override;

		[[nodiscard]] const Deinflector& deinflector() const noexcept override
		{
			return deinflector_;
		}

	protected:
		Deinflector deinflector_;

	private:
		// The language's fold of a letter, if it has one.
		[[nodiscard]] const Fold* foldOf( char32_t c ) const noexcept;
	};

	// Combining stress marks (acute and grave), which learner's texts put on vowels: кни́га.
	[[nodiscard]] constexpr bool isStressMark( char32_t c ) noexcept
	{
		return c == 0x0300 || c == 0x0301;
	}

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_ALPHABETIC_H
