#include "Defined.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/WordRules.h>
#include <lexiglance/language/Alphabetic.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <unordered_map>

namespace lexiglance::lang
{

	namespace
	{

		// Rules for the text as written (bits 24-31 are the deinflector's), unless they name the class they follow.
		constexpr std::uint32_t surface = 1U << 31U;

		// "0400-04FF" or "3005", in hexadecimal.
		std::optional<std::pair<char32_t, char32_t>> codeRange( std::string_view text )
		{
			const auto number = []( std::string_view digits ) -> std::optional<char32_t> {
				std::uint32_t value      = 0;
				const auto [end, failed] = std::from_chars( digits.data(), digits.data() + digits.size(), value, 16 );
				if ( digits.empty() || failed != std::errc{} || end != digits.data() + digits.size() || value > 0x10FFFF )
				{
					return std::nullopt;
				}
				return static_cast<char32_t>( value );
			};
			const auto dash  = text.find( '-' );
			const auto first = number( text.substr( 0, dash ) );
			const auto last  = dash == std::string_view::npos ? first : number( text.substr( dash + 1 ) );
			if ( !first || !last || *last < *first )
			{
				return std::nullopt;
			}
			return std::pair{ *first, *last };
		}

		// A language defined by its file: its script, spellings, samples, models and suffix rules.
		class DefinedLanguage final : public AlphabeticLanguage
		{
		public:
			static Result<std::unique_ptr<Language>> define( const json::Value& root );

			[[nodiscard]] std::string_view code() const noexcept override
			{
				return code_;
			}

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return name_;
			}

			[[nodiscard]] bool isScriptCharacter( char32_t c ) const noexcept override
			{
				return std::ranges::any_of( script_, [c]( const auto& range ) { return c >= range.first && c <= range.second; } );
			}

			[[nodiscard]] bool isLookupCharacter( char32_t c ) const noexcept override
			{
				return isScriptCharacter( c ) || isStressMark( c ) || word_characters_.contains( c );
			}

			[[nodiscard]] bool separatesWords() const noexcept override
			{
				return spaces_;
			}

			[[nodiscard]] std::span<const Fold> folds() const noexcept override
			{
				return folds_;
			}

			[[nodiscard]] std::string_view sampleText() const noexcept override
			{
				return sample_text_;
			}

			[[nodiscard]] std::span<const std::string_view> sampleWords() const noexcept override
			{
				return sample_words_;
			}

			[[nodiscard]] std::string_view exampleSentence() const noexcept override
			{
				return example_sentence_.empty() ? std::string_view( sample_text_ ) : std::string_view( example_sentence_ );
			}

			[[nodiscard]] std::string_view commonsPrefix() const noexcept override
			{
				return commons_prefix_;
			}

			[[nodiscard]] OcrModels ocrModels() const noexcept override
			{
				return { .paddle = paddle_, .tesseract = tesseract_, .tesseract_vertical = tesseract_vertical_ };
			}

		private:
			std::string                                code_;
			std::string                                name_;
			std::vector<std::pair<char32_t, char32_t>> script_;
			std::u32string                             word_characters_;
			bool                                       spaces_ = true;
			std::vector<Fold>                          folds_;
			std::string                                sample_text_;
			std::string                                example_sentence_;
			std::vector<std::string>                   sample_storage_;
			std::vector<std::string_view>              sample_words_;
			std::string                                commons_prefix_;
			std::string                                paddle_;
			std::string                                tesseract_;
			std::string                                tesseract_vertical_;
		};

		Result<std::unique_ptr<Language>> DefinedLanguage::define( const json::Value& root )
		{
			auto language   = std::make_unique<DefinedLanguage>();
			language->code_ = root["code"].asString();
			if ( language->code_.empty() )
			{
				return fail( R"(a language file needs a "code")" );
			}
			const std::string& code = language->code_;
			language->name_         = root["name"].asString( code );

			for ( const json::Value& item : root["script"].items() )
			{
				const auto range = codeRange( item.asString() );
				if ( !range )
				{
					return fail( R"({}: "{}" is not a range of code points such as "0400-04FF")", code, item.asString() );
				}
				language->script_.push_back( *range );
			}
			if ( language->script_.empty() )
			{
				return fail( R"({}: a language file needs its "script")", code );
			}

			language->word_characters_ = utf8::toUtf32( root["word_characters"].asString() );
			// Scripts written without spaces between words: Chinese characters, kana, Thai, Lao, Tibetan, Myanmar, Khmer.
			constexpr std::array<std::pair<char32_t, char32_t>, 9> unspaced{
				{ { 0x0E00, 0x0EFF }, { 0x0F00, 0x0FFF }, { 0x1000, 0x109F }, { 0x1780, 0x17FF }, { 0x3040, 0x30FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xF900, 0xFAFF }, { 0x20000, 0x3FFFF } }
			};
			const bool written_unspaced = std::ranges::any_of( language->script_, [&]( const auto& range ) {
				return std::ranges::any_of( unspaced, [&]( const auto& other ) { return range.first <= other.second && other.first <= range.second; } );
			} );

			language->spaces_ = root["spaces"].asBool( !written_unspaced );
			for ( const json::Member& fold : root["folds"].members() )
			{
				const auto from = utf8::toUtf32( fold.key );
				const auto to   = utf8::toUtf32( fold.value.asString() );
				if ( from.size() != 1 || to.size() > 1 )
				{
					return fail( R"({}: a fold turns one letter into one letter (or none), not "{}" into "{}")", code, fold.key, fold.value.asString() );
				}
				language->folds_.emplace_back( from.front(), to.empty() ? 0 : to.front() );
			}

			for ( const json::Value& word : root["sample_words"].items() )
			{
				language->sample_storage_.emplace_back( word.asString() );
			}
			for ( const std::string& word : language->sample_storage_ )
			{
				language->sample_words_.emplace_back( word );
			}
			language->sample_text_ = root["sample_text"].asString();
			if ( language->sample_text_.empty() )
			{
				for ( const std::string& word : language->sample_storage_ )
				{
					language->sample_text_.append( language->sample_text_.empty() ? "" : " " ).append( word );
				}
			}
			language->example_sentence_   = root["example_sentence"].asString();
			language->commons_prefix_     = root["commons_prefix"].asString();
			language->paddle_             = root["ocr"]["paddle"].asString();
			language->tesseract_          = root["ocr"]["tesseract"].asString();
			language->tesseract_vertical_ = root["ocr"]["tesseract_vertical"].asString();

			// Suffix rules: an ending and the one of the dictionary form, with the word classes the result must have.
			std::unordered_map<std::string, std::uint16_t> forms;
			for ( const json::Value& rule : root["rules"].items() )
			{
				const auto inflected  = rule["inflected"].asString();
				const auto word_class = rule["class"].asString();
				const auto after      = rule["after"].asString();
				const auto classes    = word_class.empty() ? dict::rule::dictionary_mask : dict::rule::parse( word_class );
				const auto previous   = after.empty() ? 0U : dict::rule::parse( after );
				if ( inflected.empty() || classes == 0 || ( !after.empty() && previous == 0 ) )
				{
					return fail( R"({}: the rule for "{}" needs an ending and known word classes (n, v, adj, ...))", code, inflected );
				}
				const std::string form( rule["form"].asString( "inflected form" ) );
				auto [it, added] = forms.try_emplace( form, std::uint16_t{ 0 } );
				if ( added )
				{
					it->second = language->deinflector_.addTransform( form, form );
				}
				language->deinflector_.addRule( inflected, rule["dictionary"].asString(), surface | previous, classes, it->second );
			}
			language->deinflector_.setMinimumStem( static_cast<std::size_t>( std::max<std::int64_t>( 0, root["minimum_stem"].asInt( 0 ) ) ) );
			language->deinflector_.finalize();
			language->recommend( recommendationsOf( root ) );
			language->setTranslationModel( translationModelOf( root ) );
			language->setDistinctiveLetters( distinctiveLettersOf( root ) );
			return language;
		}

	} // namespace

	Result<std::unique_ptr<Language>> defineLanguage( const json::Value& root )
	{
		return DefinedLanguage::define( root );
	}

	std::u32string distinctiveLettersOf( const json::Value& root )
	{
		return utf8::toUtf32( root["distinctive_letters"].asString() );
	}

	TranslationModel translationModelOf( const json::Value& root )
	{
		const json::Value& model = root["translation"];
		TranslationModel   out{ .repository = std::string( model["model"].asString() ), .revision = std::string( model["revision"].asString( "main" ) ), .bytes = 0, .full_bytes = 0 };
		out.bytes      = static_cast<std::uint64_t>( std::max<std::int64_t>( 0, model["size"].asInt( 0 ) ) );
		out.full_bytes = static_cast<std::uint64_t>( std::max<std::int64_t>( 0, model["full_size"].asInt( 0 ) ) );
		// A repository is "owner/name": nothing that could leave the models' directory.
		if ( out.repository.find( ".." ) != std::string::npos || out.directory().empty() || out.revision.find( '/' ) != std::string::npos )
		{
			return {};
		}
		return out;
	}

	std::vector<Recommendation> recommendationsOf( const json::Value& root )
	{
		std::vector<Recommendation> out;
		for ( const json::Value& item : root["dictionaries"].items() )
		{
			out.push_back(
					{
							.name         = std::string( item["name"].asString() ),
							.category     = std::string( item["category"].asString() ),
							.title        = std::string( item["title"].asString() ),
							.title_prefix = std::string( item["title_prefix"].asString() ),
							.description  = std::string( item["description"].asString() ),
							.homepage     = std::string( item["homepage"].asString() ),
							.download     = std::string( item["download"].asString() ),
					}
			);
		}
		return out;
	}

} // namespace lexiglance::lang
