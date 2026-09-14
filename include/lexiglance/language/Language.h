#ifndef LEXIGLANCE_LANGUAGE_LANGUAGE_H
#define LEXIGLANCE_LANGUAGE_LANGUAGE_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A language is a file in data/languages (or in the user's languagesDirectory()) and, when it needs code, a class
// derived from Language. docs/languages.md explains how to add one.
namespace lexiglance::lang
{

	class Deinflector;

	// A normalised spelling of the scanned text. The first k code points of `text` end at byte `offsets[k]` and cover
	// `source_length[k]` code points of the original text, so matches map back onto what is on screen.
	struct TextVariant
	{
		std::string                text;
		std::vector<std::uint32_t> offsets;
		std::vector<std::uint32_t> source_length;
	};

	// A piece of a headword and the reading shown above it (empty: none), e.g. 食(た) べ 物(もの).
	struct RubySegment
	{
		std::string text;
		std::string reading;
	};

	// A letter also searched as another (ё as е); 0 leaves it out.
	using Fold = std::pair<char32_t, char32_t>;

	// Models that read the language from pixels. Empty names: the default models read it.
	struct OcrModels
	{
		// A PaddleOCR recognition model, as a path in RapidOCR's model release ("PP-OCRv5/rec/eslav_PP-OCRv5_rec_mobile.onnx").
		std::string_view paddle;
		// Tesseract's names for the language's horizontal and (if it has any) vertical text: "jpn", "jpn_vert".
		std::string_view tesseract;
		std::string_view tesseract_vertical;

		// Where the PaddleOCR recogniser is kept, next to the default det.onnx and rec.onnx; languages sharing a model
		// (Russian and Ukrainian) share the file.
		[[nodiscard]] std::string paddleFile() const
		{
			const auto slash = paddle.rfind( '/' );
			return std::string( "rec-" ).append( paddle.substr( slash == std::string_view::npos ? 0 : slash + 1 ) );
		}
	};

	// A dictionary the settings application offers for a language.
	struct Recommendation
	{
		std::string name;
		std::string category;
		// Installed under exactly this title, or a title starting with the prefix.
		std::string title;
		std::string title_prefix;
		std::string description;
		std::string homepage;
		std::string download;
	};

	class Language
	{
	public:
		Language()                             = default;
		Language( const Language& )            = delete;
		Language& operator=( const Language& ) = delete;
		Language( Language&& )                 = delete;
		Language& operator=( Language&& )      = delete;
		virtual ~Language()                    = default;

		// ISO 639-1 code ("ja") and English name ("Japanese").
		[[nodiscard]] virtual std::string_view code() const noexcept = 0;
		[[nodiscard]] virtual std::string_view name() const noexcept = 0;

		// Characters of the language's own script. Text starting with one is looked up in this language, so a language
		// written in a script of its own needs no setting at all.
		[[nodiscard]] virtual bool isScriptCharacter( char32_t c ) const noexcept = 0;

		// Scanning stops at the first character for which this is false. By default: the script's characters.
		[[nodiscard]] virtual bool isLookupCharacter( char32_t c ) const noexcept;

		// Appends the spellings to search, the (width normalised) source first. By default: the text as written.
		virtual void variants( std::u32string_view source, std::vector<TextVariant>& out ) const;

		// Rules turning inflected words into dictionary forms. By default none: dictionaries that list inflected forms
		// (Wiktionary's, from wty) still find them.
		[[nodiscard]] virtual const Deinflector& deinflector() const noexcept;

		// How a headword is shown: its pieces, each with the reading above it. By default a reading that only adds stress
		// marks (кни́га for книга) is shown instead of the expression, and any other reading above the whole expression.
		[[nodiscard]] virtual std::vector<RubySegment> headword( std::string_view expression, std::string_view reading ) const;

		// Letters also searched as another (ё as е, ά as α). Dictionaries index their terms that way too.
		[[nodiscard]] virtual std::span<const Fold> folds() const noexcept
		{
			return {};
		}

		// A line in the language, to check fonts with.
		[[nodiscard]] virtual std::string_view sampleText() const noexcept = 0;

		// Everyday words (inflected ones welcome) that every general dictionary has, to test lookups and OCR with.
		[[nodiscard]] virtual std::span<const std::string_view> sampleWords() const noexcept = 0;

		// Pronunciations on Wikimedia Commons are named "<prefix>-<word>.ogg" ("Ru-книга.ogg"); empty if there are none.
		[[nodiscard]] virtual std::string_view commonsPrefix() const noexcept
		{
			return {};
		}

		[[nodiscard]] virtual OcrModels ocrModels() const noexcept
		{
			return {};
		}

		// The dictionaries offered for the language, from its file.
		[[nodiscard]] std::span<const Recommendation> recommendedDictionaries() const noexcept
		{
			return recommended_;
		}

		void recommend( std::vector<Recommendation> dictionaries )
		{
			recommended_ = std::move( dictionaries );
		}

	private:
		std::vector<Recommendation> recommended_;
	};

	// Every supported language, read once: Japanese and Russian (which come as code), then the files built in from
	// data/languages, then the user's files in languagesDirectory(). A file for a language already there replaces it.
	[[nodiscard]] std::span<const Language* const> languages();

	[[nodiscard]] const Language* findLanguage( std::string_view code );

	// Where the user's own language files are read from (<data directory>/languages).
	[[nodiscard]] std::filesystem::path languagesDirectory();

	// A language defined by a file (docs/languages.md), or what is wrong with it.
	[[nodiscard]] Result<std::unique_ptr<Language>> parseLanguage( std::string_view json );

	// The supported languages but those whose codes are in `disabled` (every one, if that would leave none). Fewer
	// languages to tell apart make detection and OCR faster.
	[[nodiscard]] std::vector<const Language*> enabledLanguages( std::span<const std::string> disabled );

	// The language to look `text` up in, among `among` (empty: every language): the one whose script its first character
	// belongs to (`preferred` when several share the script), otherwise `preferred`, otherwise the first one.
	[[nodiscard]] const Language& languageOf( std::string_view text, const Language* preferred = nullptr, std::span<const Language* const> among = {} );

	// Whether `text` starts in the script of one of `among` (empty: every language), and not, say, in English interface
	// text.
	[[nodiscard]] bool startsInKnownScript( std::string_view text, std::span<const Language* const> among = {} );

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_LANGUAGE_H
