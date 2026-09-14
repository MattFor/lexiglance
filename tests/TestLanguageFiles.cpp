#include "Test.h"

#include <lexiglance/dictionary/WordRules.h>
#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>

#include <algorithm>

// Languages defined by files: what a file can say, and what is refused.
namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	const test::Registrar language_file( "a language defined by a file", [] {
		auto defined = lg::lang::parseLanguage( R"({
			"code": "xx", "name": "Testish", "script": ["0400-04FF", "0500"], "word_characters": "-",
			"folds": {"ё": "е"}, "sample_words": ["кот", "книга"], "commons_prefix": "Xx",
			"ocr": {"paddle": "PP-OCRv5/rec/eslav_PP-OCRv5_rec_mobile.onnx", "tesseract": "xxx"},
			"minimum_stem": 2,
			"rules": [
				{"inflected": "ами", "dictionary": "а", "class": "n", "form": "instrumental plural"},
				{"inflected": "ого", "dictionary": "ый", "class": "adj", "form": "genitive"}
			],
			"dictionaries": [{"name": "X", "category": "Terms", "title": "x", "download": "https://example.invalid/x.zip"}]
		})" );
		if ( !test::expect( defined.has_value() ) )
		{
			return;
		}
		const auto& language = **defined;
		test::expect( language.code() == "xx" && language.name() == "Testish" );
		test::expect( language.isScriptCharacter( U'ж' ) && language.isScriptCharacter( 0x0500 ) && !language.isScriptCharacter( U'a' ) );
		test::expect( language.isLookupCharacter( U'-' ) && !language.isLookupCharacter( U' ' ) );
		test::expect( language.sampleWords().size() == 2 && language.sampleText() == "кот книга" && language.commonsPrefix() == "Xx" );
		test::expect( language.ocrModels().tesseract == "xxx" && language.ocrModels().paddleFile() == "rec-eslav_PP-OCRv5_rec_mobile.onnx" );
		test::expect( language.folds().size() == 1 && language.recommendedDictionaries().size() == 1 && language.recommendedDictionaries().front().title == "x" );

		std::vector<lg::lang::Deinflection> results;
		language.deinflector().deinflect( "книгами", results );
		test::expect( std::ranges::any_of( results, []( const auto& d ) { return d.text == "книга" && ( d.conditions & lg::dict::rule::noun ) != 0; } ) );
		// Folded spellings are searched: ЁЛКИ in lower case and with е.
		std::vector<lg::lang::TextVariant> variants;
		language.variants( U"ЁЛКИ", variants );
		test::expect( std::ranges::any_of( variants, []( const auto& v ) { return v.text == "елки"; } ) );
	} );

	const test::Registrar broken_language_files( "mistakes in a language file are reported", [] {
		test::expect( !lg::lang::parseLanguage( R"({"name": "No code", "script": ["0400-04FF"]})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( R"({"code": "xx"})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( R"({"code": "xx", "script": ["zz"]})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( R"({"code": "xx", "script": ["0500-0400"]})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( R"({"code": "xx", "script": ["0400"], "folds": {"ab": "c"}})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( R"({"code": "xx", "script": ["0400"], "rules": [{"inflected": "a", "dictionary": "b", "class": "nonsense"}]})" ).has_value() );
		test::expect( !lg::lang::parseLanguage( "not json" ).has_value() );
	} );

	const test::Registrar builtin_language_files( "the built-in languages and their dictionaries", [] {
		for ( const std::string_view code : { "ja", "ru", "uk", "ko", "el" } )
		{
			const auto* language = lg::lang::findLanguage( code );
			test::expect( language != nullptr && !language->recommendedDictionaries().empty() && !language->sampleWords().empty() );
		}
		test::expectEqual( lg::lang::languages().front()->code(), std::string_view( "ja" ) );
		test::expect( lg::lang::languagesDirectory().filename() == "languages" );
	} );

} // namespace
