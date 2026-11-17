#include "Test.h"

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/dictionary/SearchKey.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/Translator.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

// Ukrainian, Korean and Greek: languages that come with little more than their script.
namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	const lg::lang::Language& language( std::string_view code )
	{
		return *lg::lang::findLanguage( code );
	}

	// A dictionary written for the test: one term bank of [expression, reading] pairs.
	std::shared_ptr<const lg::dict::Dictionary> compiled( std::string_view name, std::string_view source_language, std::string_view terms )
	{
		const auto directory = test::scratch( name );
		std::ofstream( directory / "index.json" ) << R"({"title": "Test", "revision": "1", "format": 3, "sourceLanguage": ")" << source_language << R"(", "targetLanguage": "en"})";
		std::ofstream( directory / "term_bank_1.json" ) << terms;
		const auto output = directory / "out" / "test.lgd";
		if ( !test::expect( lg::dict::compile( directory, output ).has_value() ) )
		{
			return nullptr;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		if ( !test::expect( dictionary.has_value() ) )
		{
			return nullptr;
		}
		return *dictionary;
	}

	std::shared_ptr<const lg::lookup::DictionarySet> dictionaryOf( std::string_view name, std::string_view source_language, std::string_view terms )
	{
		auto dictionary = compiled( name, source_language, terms );
		if ( !dictionary )
		{
			return nullptr;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.push_back( { .dictionary = std::move( dictionary ), .styles = nullptr, .name = "Test" } );
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	const test::Registrar scripts( "korean and greek told by their script, ukrainian when russian is off", [] {
		test::expectEqual( lg::lang::languageOf( "학교에서" ).code(), std::string_view( "ko" ) );
		test::expectEqual( lg::lang::languageOf( "σπίτι" ).code(), std::string_view( "el" ) );
		// Cyrillic is Russian's first; with Russian off, or Ukrainian preferred, it is Ukrainian.
		test::expectEqual( lg::lang::languageOf( "мова" ).code(), std::string_view( "ru" ) );
		test::expectEqual( lg::lang::languageOf( "мова", &language( "uk" ) ).code(), std::string_view( "uk" ) );
		const std::array<std::string, 1> no_russian{ "ru" };
		test::expectEqual( lg::lang::languageOf( "мова", nullptr, lg::lang::enabledLanguages( no_russian ) ).code(), std::string_view( "uk" ) );
		// Languages sharing a recogniser share its file.
		test::expectEqual( language( "ru" ).ocrModels().paddleFile(), language( "uk" ).ocrModels().paddleFile() );
		test::expect( language( "ko" ).ocrModels().paddleFile() != language( "el" ).ocrModels().paddleFile() );
	} );

	const test::Registrar greek_keys( "greek searched without accents and final sigma", [] {
		test::expectEqual( lg::dict::searchKey( "σπίτι" ), std::string( "σπιτι" ) );
		test::expectEqual( lg::dict::searchKey( "Άνθρωπος" ), std::string( "Ανθρωποσ" ) );
		test::expect( lg::dict::matchesSearchKey( "καλημέρα", "καλημερα" ) );

		const auto set = dictionaryOf( "greek", "el", R"([["σπίτι", "", "n", "", 0, ["house"], 1, ""], ["άνθρωπος", "", "n", "", 0, ["person"], 2, ""]])" );
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		// Capitals without accents, as signs and titles write them.
		for ( const std::string_view text : { "σπίτι", "ΣΠΙΤΙ", "Σπίτι μου" } )
		{
			const auto result = translator.lookup( set, text );
			test::expect( result.language == &language( "el" ) && !result.terms.empty() && result.terms.front().expression == "σπίτι" && result.matched_length == 5 );
		}
		const auto result = translator.lookup( set, "ΑΝΘΡΩΠΟΣ" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "άνθρωπος" );
	} );

	const test::Registrar korean_words( "korean words before their particles", [] {
		const auto set = dictionaryOf( "korean", "ko", R"([["학교", "", "n", "", 0, ["school"], 1, ""], ["사람", "", "n", "", 0, ["person"], 2, ""]])" );
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		const auto             result = translator.lookup( set, "학교에서 공부해요" );
		test::expect( result.language == &language( "ko" ) && !result.terms.empty() && result.terms.front().expression == "학교" && result.matched_length == 2 );
	} );

	const test::Registrar ukrainian_words( "ukrainian with and without russian", [] {
		const auto set = dictionaryOf( "ukrainian", "uk", R"([["мова", "мо́ва", "n", "", 0, ["language"], 1, ""], ["їжа", "", "n", "", 0, ["food"], 2, ""]])" );
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		// Ukrainian dictionaries are searched whichever Cyrillic language the text is taken for.
		auto result = translator.lookup( set, "Мова" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "мова" );
		const std::array<std::string, 1> no_russian{ "ru" };
		result = translator.lookup( set, "їжа", { .languages = lg::lang::enabledLanguages( no_russian ) } );
		test::expect( result.language == &language( "uk" ) && !result.terms.empty() && result.terms.front().expression == "їжа" );
	} );

	const test::Registrar other_languages( "dictionaries of another script are not searched", [] {
		// Japanese dictionaries carry the odd Russian word (Pixiv's Москва, read もすくわ); Russian text finds the Russian
		// one only, and Japanese text the Japanese one. A dictionary of no known language is searched for all text.
		auto japanese = compiled( "other-ja", "ja", R"json([["Москва", "もすくわ", "n", "", 0, ["Moscow (article)"], 1, ""], ["東京", "とうきょう", "n", "", 0, ["Tokyo"], 2, ""]])json" );
		auto russian  = compiled( "other-ru", "ru", R"json([["Москва", "", "n", "", 0, ["Moscow"], 1, ""], ["東京", "", "n", "", 0, ["Tokyo (in Russian)"], 2, ""]])json" );
		auto unknown  = compiled( "other-none", "", R"json([["Tokyo", "", "n", "", 0, ["Tokyo"], 1, ""], ["Moscow", "", "n", "", 0, ["Moscow"], 2, ""], ["東京", "", "n", "", 0, ["Tokyo (unknown)"], 3, ""]])json" );
		if ( !test::expect( japanese && russian && unknown ) )
		{
			return;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		for ( const auto& dictionary : { japanese, russian, unknown } )
		{
			loaded.push_back( { .dictionary = dictionary, .styles = nullptr, .name = "Test" } );
		}
		const auto             set = std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
		lg::lookup::Translator translator;
		const auto             from = [&]( std::string_view text ) {
			std::vector<std::uint16_t> dictionaries;
			for ( const auto& entry : translator.lookup( set, text ).terms )
			{
				for ( const auto& definition : entry.definitions )
				{
					dictionaries.push_back( definition.dictionary );
				}
			}
			std::ranges::sort( dictionaries );
			return dictionaries;
		};
		test::expect( from( "Москва" ) == std::vector<std::uint16_t>{ 1 } );
		test::expect( from( "東京" ) == std::vector<std::uint16_t>{ 0, 2 } );
	} );

} // namespace
