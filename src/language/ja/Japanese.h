#ifndef LEXIGLANCE_LANGUAGE_JA_JAPANESE_H
#define LEXIGLANCE_LANGUAGE_JA_JAPANESE_H

#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>

namespace lexiglance::lang::ja
{

	class Japanese final : public Language
	{
	public:
		Japanese();

		[[nodiscard]] std::string_view code() const noexcept override
		{
			return "ja";
		}

		[[nodiscard]] std::string_view name() const noexcept override
		{
			return "Japanese";
		}

		// Kana, kanji, and the brackets Japanese text opens with.
		[[nodiscard]] bool isScriptCharacter( char32_t c ) const noexcept override;

		// Also Latin letters and digits, which Japanese words contain (Tシャツ, CD).
		[[nodiscard]] bool isLookupCharacter( char32_t c ) const noexcept override;

		[[nodiscard]] bool separatesWords() const noexcept override
		{
			return false;
		}

		void variants( std::u32string_view source, std::vector<TextVariant>& out ) const override;

		[[nodiscard]] const Deinflector& deinflector() const noexcept override
		{
			return deinflector_;
		}

		// Furigana above the kanji only: 食(た) べ 物(もの).
		[[nodiscard]] std::vector<RubySegment> headword( std::string_view expression, std::string_view reading ) const override;

		[[nodiscard]] std::string_view sampleText() const noexcept override
		{
			return "日本語の辞書、ひらがな、カタカナ";
		}

		[[nodiscard]] std::string_view exampleSentence() const noexcept override
		{
			return "私は昨日、友達と一緒に図書館で本を読みました。";
		}

		[[nodiscard]] std::span<const std::string_view> sampleWords() const noexcept override;

		[[nodiscard]] std::string_view commonsPrefix() const noexcept override
		{
			return "Ja";
		}

		// PaddleOCR's default recogniser reads Japanese.
		[[nodiscard]] OcrModels ocrModels() const noexcept override
		{
			return { .paddle = {}, .tesseract = "jpn", .tesseract_vertical = "jpn_vert" };
		}

	private:
		Deinflector deinflector_;
	};

} // namespace lexiglance::lang::ja

#endif // LEXIGLANCE_LANGUAGE_JA_JAPANESE_H
