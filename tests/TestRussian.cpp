#include "Test.h"

#include <lexiglance/dictionary/Classify.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/dictionary/SearchKey.h>
#include <lexiglance/dictionary/WordRules.h>
#include <lexiglance/language/Case.h>
#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/lookup/Translator.h>

#include <algorithm>
#include <array>

// Russian, and what makes a language pluggable: detection by script, languages turned off, search keys, form-of
// entries of Wiktionary's dictionaries.
namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;
	namespace rule = lexiglance::dict::rule;

	const lg::lang::Language& russian()
	{
		return *lg::lang::findLanguage( "ru" );
	}

	const lg::lang::Language& japanese()
	{
		return *lg::lang::findLanguage( "ja" );
	}

	std::filesystem::path russianFixture()
	{
		return test::fixtureDirectory().parent_path() / "fixture-ru";
	}

	// The chain that deinflects `text` into `expected` with a word class in `classes`, or "-" if none does.
	std::string deinflectsTo( std::string_view text, std::string_view expected, std::uint32_t classes )
	{
		std::vector<lg::lang::Deinflection> results;
		russian().deinflector().deinflect( text, results );
		for ( const auto& d : results )
		{
			if ( d.text == expected && ( d.conditions & classes ) != 0 )
			{
				std::string out;
				for ( std::size_t i = d.length; i-- > 0; )
				{
					out.append( out.empty() ? "" : " « " ).append( russian().deinflector().transformName( d.chain[i] ) );
				}
				return out;
			}
		}
		return "-";
	}

	const test::Registrar russian_nouns( "russian noun declension", [] {
		test::expectEqual( deinflectsTo( "книги", "книга", rule::noun ), std::string( "genitive singular or nominative plural" ) );
		test::expectEqual( deinflectsTo( "книгами", "книга", rule::noun ), std::string( "instrumental plural" ) );
		test::expectEqual( deinflectsTo( "книг", "книга", rule::noun ), std::string( "genitive plural" ) );
		test::expectEqual( deinflectsTo( "столами", "стол", rule::noun ), std::string( "instrumental plural" ) );
		test::expectEqual( deinflectsTo( "ночью", "ночь", rule::noun ), std::string( "instrumental singular" ) );
		test::expectEqual( deinflectsTo( "зданий", "здание", rule::noun ), std::string( "genitive plural" ) );
		test::expectEqual( deinflectsTo( "куска", "кусок", rule::noun ), std::string( "genitive singular" ) );
		test::expectEqual( deinflectsTo( "времени", "время", rule::noun ), std::string( "genitive/dative/prepositional singular" ) );
		// One-letter words (в, к) do not inflect: a lone letter is no stem.
		test::expectEqual( deinflectsTo( "к", "ка", rule::noun ), std::string( "-" ) );
		test::expect( russian().deinflector().ruleCount() > 400 );
	} );

	const test::Registrar russian_adjectives( "russian adjective declension", [] {
		test::expectEqual( deinflectsTo( "красивого", "красивый", rule::adjective ), std::string( "genitive singular" ) );
		test::expectEqual( deinflectsTo( "хорошая", "хороший", rule::adjective ), std::string( "feminine" ) );
		test::expectEqual( deinflectsTo( "синего", "синий", rule::adjective ), std::string( "genitive singular" ) );
		test::expectEqual( deinflectsTo( "большие", "большой", rule::adjective ), std::string( "plural" ) );
		test::expectEqual( deinflectsTo( "красива", "красивый", rule::adjective ), std::string( "short form" ) );
		test::expectEqual( deinflectsTo( "быстрее", "быстрый", rule::adjective ), std::string( "comparative" ) );
		test::expectEqual( deinflectsTo( "красивейшего", "красивый", rule::adjective ), std::string( "superlative « genitive singular" ) );
	} );

	const test::Registrar russian_verbs( "russian verb conjugation", [] {
		test::expectEqual( deinflectsTo( "читаю", "читать", rule::verb ), std::string( "first person singular" ) );
		test::expectEqual( deinflectsTo( "говоришь", "говорить", rule::verb ), std::string( "second person singular" ) );
		test::expectEqual( deinflectsTo( "учится", "учиться", rule::verb ), std::string( "third person singular" ) );
		test::expectEqual( deinflectsTo( "читала", "читать", rule::verb ), std::string( "past" ) );
		test::expectEqual( deinflectsTo( "рисую", "рисовать", rule::verb ), std::string( "first person singular" ) );
		test::expectEqual( deinflectsTo( "иду", "идти", rule::verb ), std::string( "first person singular" ) );
		test::expectEqual( deinflectsTo( "люблю", "любить", rule::verb ), std::string( "first person singular" ) );
		test::expectEqual( deinflectsTo( "читай", "читать", rule::verb ), std::string( "imperative" ) );
		test::expectEqual( deinflectsTo( "улыбаясь", "улыбаться", rule::verb ), std::string( "gerund" ) );
		test::expectEqual( deinflectsTo( "читающего", "читать", rule::verb ), std::string( "present active participle « genitive singular" ) );
		test::expectEqual( deinflectsTo( "прочитанный", "прочитать", rule::verb ), std::string( "past passive participle" ) );
	} );

	const test::Registrar letter_case( "letter case of Latin, Greek and Cyrillic", [] {
		test::expect( lg::lang::lowercase( U'Ё' ) == U'ё' && lg::lang::uppercase( U'ж' ) == U'Ж' && lg::lang::lowercase( U'Я' ) == U'я' );
		test::expect( lg::lang::lowercase( U'Σ' ) == U'σ' && lg::lang::uppercase( U'ς' ) == U'Σ' && lg::lang::uppercase( U'ά' ) == U'Ά' );
		test::expect( lg::lang::lowercase( U'Ā' ) == U'ā' && lg::lang::lowercase( U'İ' ) == U'i' && lg::lang::uppercase( U'ı' ) == U'I' && lg::lang::uppercase( U'ÿ' ) == U'Ÿ' );
		test::expect( lg::lang::lowercase( U'A' ) == U'a' && lg::lang::lowercase( U'×' ) == U'×' && lg::lang::lowercase( U'あ' ) == U'あ' );
		test::expect( lg::lang::isUppercase( U'Ґ' ) && !lg::lang::isUppercase( U'ґ' ) );
	} );

	const test::Registrar russian_variants( "russian spellings searched", [] {
		std::vector<lg::lang::TextVariant> variants;
		russian().variants( U"Кни́ги", variants );
		const auto has = [&]( std::string_view text ) { return std::ranges::any_of( variants, [&]( const auto& v ) { return v.text == text; } ); };
		test::expect( has( "Книги" ) && has( "книги" ) );
		// The stress mark goes, and the letter before it covers it on screen.
		test::expectEqual( variants.front().source_length.back(), std::uint32_t{ 6 } );

		variants.clear();
		russian().variants( U"ЁЛКИ", variants );
		test::expect( has( "ЁЛКИ" ) && has( "ёлки" ) && has( "Ёлки" ) && has( "елки" ) );
	} );

	const test::Registrar language_detection( "language told by the script", [] {
		test::expectEqual( lg::lang::languageOf( "книга" ).code(), std::string_view( "ru" ) );
		test::expectEqual( lg::lang::languageOf( "食べる" ).code(), std::string_view( "ja" ) );
		test::expectEqual( lg::lang::languageOf( "「本」" ).code(), std::string_view( "ja" ) );
		// Text in no language's script goes to the preferred one.
		test::expectEqual( lg::lang::languageOf( "hello", &russian() ).code(), std::string_view( "ru" ) );
		test::expect( lg::lang::startsInKnownScript( "книга" ) && lg::lang::startsInKnownScript( "カタカナ" ) && !lg::lang::startsInKnownScript( "Settings" ) );

		// Languages turned off are not told apart any more; turning all off keeps them all.
		std::vector<std::string> all;
		for ( const auto* language : lg::lang::languages() )
		{
			all.emplace_back( language->code() );
		}
		std::vector<std::string> all_but_russian = all;
		std::erase( all_but_russian, "ru" );
		const auto only_russian = lg::lang::enabledLanguages( all_but_russian );
		test::expect( only_russian.size() == 1 && only_russian.front() == &russian() );
		test::expect( !lg::lang::startsInKnownScript( "食べる", only_russian ) );
		test::expectEqual( lg::lang::languageOf( "食べる", &japanese(), only_russian ).code(), std::string_view( "ru" ) );
		test::expectEqual( lg::lang::enabledLanguages( all ).size(), lg::lang::languages().size() );
	} );

	const test::Registrar search_keys( "search keys of spellings", [] {
		test::expectEqual( lg::dict::searchKey( "ёлка" ), std::string( "елка" ) );
		test::expectEqual( lg::dict::searchKey( "Кни́га" ), std::string( "Книга" ) );
		test::expectEqual( lg::dict::searchKey( "ЁЛКА" ), std::string( "ЕЛКА" ) );
		test::expect( lg::dict::searchKey( "食べる" ).empty() && lg::dict::searchKey( "книга" ).empty() );
		test::expect( lg::dict::matchesSearchKey( "ёлка", "елка" ) && lg::dict::matchesSearchKey( "стол", "стол" ) && !lg::dict::matchesSearchKey( "ёлка", "ёлка" ) );
		test::expect( !lg::dict::matchesSearchKey( "кни́га", "книг" ) );
		test::expect( lg::dict::onlyAddsStress( "кни́га", "книга" ) && !lg::dict::onlyAddsStress( "книга", "книга" ) && !lg::dict::onlyAddsStress( "кни́ги", "книга" ) );
	} );

	const test::Registrar headwords( "headwords as their language writes them", [] {
		const auto stressed = russian().headword( "книга", "кни́га" );
		test::expect( stressed.size() == 1 && stressed.front().text == "кни́га" && stressed.front().reading.empty() );
		const auto plain = russian().headword( "стол", "" );
		test::expect( plain.size() == 1 && plain.front().text == "стол" );
		test::expectEqual( japanese().headword( "食べ物", "たべもの" ).size(), std::size_t{ 3 } );
	} );

	std::shared_ptr<const lg::dict::Dictionary> compiled( const std::filesystem::path& source, std::string_view name )
	{
		const auto output = test::scratch( name ) / "dictionary.lgd";
		if ( !test::expect( lg::dict::compile( source, output ).has_value() ) )
		{
			return nullptr;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		return dictionary ? *dictionary : nullptr;
	}

	std::shared_ptr<const lg::lookup::DictionarySet> setOf( std::vector<std::shared_ptr<const lg::dict::Dictionary>> dictionaries )
	{
		std::vector<lg::lookup::LoadedDictionary> loaded;
		for ( auto& dictionary : dictionaries )
		{
			if ( dictionary == nullptr )
			{
				return nullptr;
			}
			loaded.push_back( { .dictionary = std::move( dictionary ), .styles = nullptr, .name = "Fixture" } );
		}
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	const test::Registrar search_key_index( "terms found by their search key", [] {
		const auto dictionary = compiled( russianFixture(), "russian-index" );
		if ( !test::expect( dictionary != nullptr ) )
		{
			return;
		}
		const auto& d        = *dictionary;
		const auto  contains = [&]( std::string_view key, std::string_view expression ) {
			return std::ranges::any_of( d.findTerms( key ), [&]( std::uint32_t index ) { return d.string( d.terms()[index].expression ) == expression; } );
		};
		test::expect( contains( "ёлка", "ёлка" ) && contains( "елка", "ёлка" ) && contains( "идет", "идёт" ) );
		test::expect( contains( "книга", "книга" ) && contains( "кни́га", "книга" ) );
		// Part of speech from Wiktionary's tags.
		const auto terms = d.findTerms( "стол" );
		test::expect( std::ranges::any_of( terms, [&]( std::uint32_t index ) { return ( d.terms()[index].rules & rule::noun ) != 0; } ) );

		const auto profile = lg::dict::profile( d );
		test::expectEqual( profile.language, std::string( "ru" ) );
		test::expectEqual( lg::dict::dictionaryLanguage( d ), std::string( "ru" ) );
	} );

	const test::Registrar russian_lookups( "russian lookups", [] {
		const auto set = setOf( { compiled( russianFixture(), "russian-lookups" ) } );
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		const auto             front = [&]( std::string_view text ) {
			auto result = translator.lookup( set, text );
			test::expect( result.language == &russian() );
			return result;
		};

		// A form-of entry stands for its word, saying what the text is of it.
		auto result = front( "Книги на столе" );
		if ( test::expect( !result.terms.empty() ) )
		{
			test::expectEqual( result.terms.front().expression, std::string_view( "книга" ) );
			test::expectEqual( result.matched_length, std::uint32_t{ 5 } );
			test::expectEqual( result.inflectionText( result.terms.front() ), std::string( "genitive singular, nominative plural" ) );
			const auto headword = result.headword( result.terms.front() );
			test::expect( headword.size() == 1 && headword.front().text == "кни́га" );
		}

		// A form of a form: читающего -> читающий -> читать.
		result = front( "читающего" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "читать" && result.inflectionText( result.terms.front() ) == "present active participle « genitive masculine" );

		// Written without ё, pointing at a word with a stress mark.
		result = front( "идет" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "идти" && result.inflectionText( result.terms.front() ) == "third-person singular present" );

		// Words without listed forms are found by the rules, with or without ё.
		for ( const std::string_view text : { "ёлки", "елки", "ЁЛКИ" } )
		{
			result = front( text );
			test::expect( !result.terms.empty() && result.terms.front().expression == "ёлка" && result.matched_length == 4 );
		}
		result = front( "красивого" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "красивый" );
		result = front( "столами" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "стол" );
		result = front( "учится" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "учиться" );

		// A form-of entry whose word no dictionary has comes after what the rules find.
		result = front( "стола" );
		if ( test::expect( result.terms.size() >= 2 ) )
		{
			test::expectEqual( result.terms.front().expression, std::string_view( "стол" ) );
			test::expect( std::ranges::any_of( result.terms, []( const auto& t ) { return t.expression == "стола" && t.unresolved; } ) );
		}

		// The spelling as written comes first; the other one is still there.
		result = front( "все" );
		test::expect( result.terms.size() >= 2 && result.terms[0].expression == "все" && result.terms[1].expression == "всё" && result.terms[1].folded );
		result = front( "всё" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "всё" );

		// Capitals: a name written in capitals, and a one-letter word.
		result = front( "МОСКВЕ" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "Москва" && result.inflectionText( result.terms.front() ) == "prepositional singular" );
		result = front( "В Москве" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "в" && result.matched_length == 1 );

		// With Russian turned off, Cyrillic text is not looked up at all.
		result = translator.lookup( set, "книги", { .language = &japanese(), .languages = { &japanese() } } );
		test::expect( result.language == &japanese() && result.terms.empty() );
	} );

	const test::Registrar mixed_languages( "japanese and russian dictionaries together", [] {
		const auto set = setOf( { compiled( test::fixtureDirectory(), "mixed-ja" ), compiled( russianFixture(), "mixed-ru" ) } );
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		auto                   result = translator.lookup( set, "食べさせられなかった" );
		test::expect( result.language == &japanese() && !result.terms.empty() && result.terms.front().expression == "食べる" );
		result = translator.lookup( set, "книгу" );
		test::expect( result.language == &russian() && !result.terms.empty() && result.terms.front().expression == "книга" );
		test::expect( std::ranges::all_of( result.terms, []( const auto& t ) { return t.definitions.front().dictionary == 1; } ) );
	} );

	const test::Registrar russian_kinds( "what a russian dictionary is for", [] {
		using lg::dict::Kind;
		test::expect( lg::dict::classify( { .title = "wty-ru-en", .source_language = "ru", .target_language = "en", .terms = 1488029 } ) == Kind::Words );
		test::expect( lg::dict::classify( { .title = "wty-ru-ru", .source_language = "ru", .target_language = "ru", .terms = 900000 } ) == Kind::Monolingual );
		test::expect( lg::dict::classify( { .title = "Толковый словарь", .language = "ru", .terms = 80000, .native_share = 0.9 } ) == Kind::Monolingual );
		test::expectEqual( lg::dict::kindName( Kind::Monolingual ), std::string_view( "Monolingual" ) );
	} );

} // namespace
