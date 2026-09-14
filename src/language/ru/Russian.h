#ifndef LEXIGLANCE_LANGUAGE_RU_RUSSIAN_H
#define LEXIGLANCE_LANGUAGE_RU_RUSSIAN_H

#include <lexiglance/language/Alphabetic.h>

namespace lexiglance::lang::ru
{

	// Russian: Cyrillic text, looked up with and without ё and stress marks. Wiktionary dictionaries list the inflected
	// forms of their words; the rules here find the dictionary form of the regular ones for dictionaries that do not.
	class Russian final : public AlphabeticLanguage
	{
	public:
		Russian();

		[[nodiscard]] std::string_view code() const noexcept override
		{
			return "ru";
		}

		[[nodiscard]] std::string_view name() const noexcept override
		{
			return "Russian";
		}

		[[nodiscard]] bool isScriptCharacter( char32_t c ) const noexcept override;

		[[nodiscard]] std::string_view sampleText() const noexcept override;

		[[nodiscard]] std::span<const std::string_view> sampleWords() const noexcept override;

		[[nodiscard]] std::string_view commonsPrefix() const noexcept override
		{
			return "Ru";
		}

		[[nodiscard]] OcrModels ocrModels() const noexcept override
		{
			return { .paddle = "PP-OCRv5/rec/eslav_PP-OCRv5_rec_mobile.onnx", .tesseract = "rus", .tesseract_vertical = {} };
		}

		// Printed Russian mostly writes ё as е, and stressed vowels have precomposed forms.
		[[nodiscard]] std::span<const Fold> folds() const noexcept override;
	};

} // namespace lexiglance::lang::ru

#endif // LEXIGLANCE_LANGUAGE_RU_RUSSIAN_H
