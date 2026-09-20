#include "Test.h"

#include <lexiglance/language/Language.h>
#include <lexiglance/translate/Model.h>
#include <lexiglance/translate/Tokenizer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	// A vocabulary as OPUS-MT's converted tokenizer.json has it, unk_id pointing at the wrong piece included.
	constexpr std::string_view tokenizer_json = R"({
		"added_tokens": [
			{"id": 0, "content": "</s>", "special": true},
			{"id": 1, "content": "<unk>", "special": true},
			{"id": 11, "content": "<pad>", "special": true}
		],
		"model": {"type": "Unigram", "unk_id": 2, "vocab": [
			["</s>", 0.0], ["<unk>", 0.0], [",", -3.0], ["▁книг", -4.0], ["у", -2.0], ["▁к", -5.0], ["ниг", -6.0],
			["▁", -1.0], ["▁hello", -2.0], ["▁world", -2.0], ["▁.", -2.5], ["<pad>", 0.0]
		]}
	})";

	const test::Registrar tokenizer_pieces( "translation tokenizer", [] {
		auto tokenizer = lg::translate::Tokenizer::parse( std::string( tokenizer_json ) );
		if ( !test::expect( tokenizer.has_value() ) )
		{
			return;
		}
		using Ids = std::vector<std::int64_t>;
		// The cut with the best total score (▁книг у, not ▁к ниг у), then the end of the sequence.
		test::expect( tokenizer->encode( "книгу" ) == Ids{ 3, 4, 0 } );
		// Words are split at whitespace, each with ▁ in front.
		test::expect( tokenizer->encode( "  книгу\tкнигу " ) == Ids{ 3, 4, 3, 4, 0 } );
		// Characters no piece covers are one unknown piece in a row, the real <unk> (not what unk_id says).
		test::expect( tokenizer->encode( "книгуXY" ) == Ids{ 3, 4, 1, 0 } );
		// Special tokens written out are only letters.
		test::expect( tokenizer->encode( "</s>" ).front() != 0 );
		test::expect( tokenizer->encode( "" ) == Ids{ 0 } );

		// Decoding leaves the special ones out and spaces English as it is written.
		const Ids written{ 8, 9, 10, 0, 11 };
		test::expectEqual( tokenizer->decode( written ), std::string( "hello world." ) );
		test::expectEqual( tokenizer->decode( Ids{ 11, 0 } ), std::string() );

		test::expect( !lg::translate::Tokenizer::parse( R"({"model": {"type": "BPE", "vocab": {}}})" ).has_value() );
		test::expect( !lg::translate::Tokenizer::parse( "not json" ).has_value() );
	} );

	const test::Registrar translation_languages( "the language a sentence is translated from", [] {
		const auto* russian   = lg::lang::findLanguage( "ru" );
		const auto* ukrainian = lg::lang::findLanguage( "uk" );
		const auto* japanese  = lg::lang::findLanguage( "ja" );
		test::expect( lg::lang::translationLanguage( "Я читаю книгу." ) == russian );
		// Letters only Ukrainian has tell it from Russian, whichever is preferred.
		test::expect( lg::lang::translationLanguage( "Україна — це держава в Європі.", russian ) == ukrainian );
		test::expect( lg::lang::translationLanguage( "Съешь же ещё этих булок.", ukrainian ) == russian );
		// Neither's letters: the preferred one.
		test::expect( lg::lang::translationLanguage( "Он там", ukrainian ) == ukrainian );
		test::expect( lg::lang::translationLanguage( "今日は図書館で本を読みました。" ) == japanese );
		// Not in any language that can be translated.
		test::expect( lg::lang::translationLanguage( "hello world" ) == nullptr );
		test::expect( lg::lang::translationLanguage( "12345 !!!" ) == nullptr );
		// Mostly English with a Russian word is still English.
		test::expect( lg::lang::translationLanguage( "the word книга means book" ) == nullptr );
	} );

	const test::Registrar model_precisions( "translation models in either precision, the wanted one first", [] {
		namespace translate  = lg::translate;
		const auto directory = test::scratch( "translation-precisions" );
		const auto place     = [&]( translate::Precision precision ) {
			for ( const std::string_view file : translate::modelFiles( precision ) )
			{
				std::ofstream( directory / file ) << "x";
			}
		};
		test::expect( !translate::downloaded( directory, translate::Precision::Full ).has_value() );
		place( translate::Precision::Compact );
		// Only the compact one: it translates, whichever is wanted.
		test::expect( translate::downloaded( directory, translate::Precision::Full ) == translate::Precision::Compact );
		test::expect( translate::downloaded( directory, translate::Precision::Compact ) == translate::Precision::Compact );
		place( translate::Precision::Full );
		test::expect( translate::downloaded( directory, translate::Precision::Full ) == translate::Precision::Full );
		test::expect( translate::precisionNamed( "full" ) == translate::Precision::Full && translate::precisionNamed( "" ) == translate::Precision::Compact );
		test::expectEqual( translate::precisionName( translate::Precision::Full ), std::string_view( "full" ) );
		test::expect( translate::otherPrecision( translate::Precision::Full ) == translate::Precision::Compact && translate::otherPrecision( translate::Precision::Compact ) == translate::Precision::Full );
		// One precision's weights alone: config.json and tokenizer.json belong to both, so they are not among them.
		for ( const std::string_view file : translate::weightFiles( translate::Precision::Full ) )
		{
			test::expect( file.ends_with( ".onnx" ) && !std::ranges::contains( translate::weightFiles( translate::Precision::Compact ), file ) );
			std::error_code ec;
			std::filesystem::remove( directory / file, ec );
		}
		// With them gone the compact ones are what is left to translate with.
		test::expect( translate::downloaded( directory, translate::Precision::Full ) == translate::Precision::Compact );
		// The files a load wants and does not find are reported as the model not being there.
		const auto missing = translate::Model::load( test::scratch( "translation-none" ), translate::Precision::Full, directory, 1 );
		test::expect( !missing && missing.error().message == translate::model_absent );
	} );

} // namespace
