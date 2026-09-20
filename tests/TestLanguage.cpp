#include "Test.h"

#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/dictionary/WordRules.h>
#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/language/ja/Furigana.h>
#include <lexiglance/language/ja/Kana.h>
#include <lexiglance/lookup/Translator.h>

#include <algorithm>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;
	namespace rule = lexiglance::dict::rule;

	const lg::lang::Language& japanese()
	{
		return *lg::lang::findLanguage( "ja" );
	}

	std::string chain( const lg::lang::Deinflection& d )
	{
		std::string out;
		for ( std::size_t i = d.length; i-- > 0; )
		{
			out.append( out.empty() ? "" : " « " ).append( japanese().deinflector().transformName( d.chain[i] ) );
		}
		return out;
	}

	// Returns the chain that deinflects `text` into `expected` with a word class in `classes`, or "-" if none does.
	std::string deinflectsTo( std::string_view text, std::string_view expected, std::uint32_t classes )
	{
		std::vector<lg::lang::Deinflection> results;
		japanese().deinflector().deinflect( text, results );
		for ( const auto& d : results )
		{
			if ( d.text == expected && ( d.conditions & classes ) != 0 )
			{
				return chain( d );
			}
		}
		return "-";
	}

	const test::Registrar verb_conjugations( "verb deinflection", [] {
		test::expectEqual( deinflectsTo( "食べさせられなかった", "食べる", rule::v1 ), std::string( "causative « potential or passive « negative « past" ) );
		test::expectEqual( deinflectsTo( "読んでいる", "読む", rule::v5 ), std::string( "-te « -teiru" ) );
		test::expectEqual( deinflectsTo( "食べませんでした", "食べる", rule::v1 ), std::string( "polite « negative « past" ) );
		test::expectEqual( deinflectsTo( "書かされる", "書く", rule::v5 ), std::string( "causative « passive" ) );
		test::expectEqual( deinflectsTo( "行った", "行く", rule::v5 ), std::string( "past" ) );
		test::expectEqual( deinflectsTo( "来ます", "来る", rule::vk ), std::string( "polite" ) );
		test::expectEqual( deinflectsTo( "しなければ", "する", rule::vs ), std::string( "negative « -ba" ) );
		test::expectEqual( deinflectsTo( "勉強しました", "勉強", rule::vs ), std::string( "suru « polite « past" ) );
		test::expectEqual( deinflectsTo( "食べちゃった", "食べる", rule::v1 ), std::string( "-chau « past" ) );
	} );

	const test::Registrar adjective_conjugations( "adjective deinflection", [] {
		test::expectEqual( deinflectsTo( "高くなかった", "高い", rule::adj_i ), std::string( "negative « past" ) );
		test::expectEqual( deinflectsTo( "高くて", "高い", rule::adj_i ), std::string( "-te" ) );
		test::expectEqual( deinflectsTo( "高すぎる", "高い", rule::adj_i ), std::string( "-sugiru" ) );
		test::expectEqual( deinflectsTo( "高ければ", "高い", rule::adj_i ), std::string( "-ba" ) );
		test::expectEqual( deinflectsTo( "高さ", "高い", rule::adj_i ), std::string( "noun" ) );
	} );

	const test::Registrar deinflection_first( "source text comes first", [] {
		std::vector<lg::lang::Deinflection> results;
		japanese().deinflector().deinflect( "食べた", results );
		test::expect( !results.empty() && results.front().text == "食べた" && results.front().conditions == 0 && results.front().length == 0 );
		test::expect( japanese().deinflector().ruleCount() > 300 );
	} );

	const test::Registrar kana_conversion( "kana conversion", [] {
		test::expectEqual( lg::lang::ja::toHiragana( "カタカナとひらがな" ), std::string( "かたかなとひらがな" ) );
		test::expectEqual( lg::lang::ja::toKatakana( "ひらがなー" ), std::string( "ヒラガナー" ) );
		test::expectEqual( lg::lang::ja::combineMark( U'か', false ), U'が' );
		test::expectEqual( lg::lang::ja::combineMark( U'ハ', true ), U'パ' );
		test::expectEqual( lg::lang::ja::combineMark( U'ウ', false ), U'ヴ' );
		test::expectEqual( lg::lang::ja::combineMark( U'あ', false ), char32_t{ 0 } );
		test::expect( lg::lang::ja::isKanji( U'々' ) && !lg::lang::ja::isKanji( U'あ' ) );
	} );

	const test::Registrar word_starts( "the start of the word under the pointer", [] {
		const auto& russian = *lg::lang::findLanguage( "ru" );
		// Where the word holding `pointed` (a byte offset into `text`) starts, as the text from there.
		const auto from = []( std::string_view text, std::string_view pointed, const lg::lang::Language& language ) {
			return std::string( text.substr( lg::lang::wordStart( text, text.find( pointed ), language ) ) );
		};
		test::expectEqual( from( "я читаю книгу", "игу", russian ), std::string( "книгу" ) );
		test::expectEqual( from( "я читаю книгу", "книгу", russian ), std::string( "книгу" ) );
		test::expectEqual( from( "я читаю книгу", "у", russian ), std::string( "книгу" ) );
		test::expectEqual( from( "кто-нибудь пришёл", "нибудь", russian ), std::string( "кто-нибудь пришёл" ) );
		// Not before the letters: a hyphen or apostrophe in front of a word is not part of it.
		test::expectEqual( from( "он -нибудь", "будь", russian ), std::string( "нибудь" ) );
		test::expectEqual( from( "«книгу»", "игу", russian ), std::string( "книгу»" ) );
		// Nothing to do on a space, on punctuation, or in a language written without spaces.
		test::expectEqual( from( "я читаю книгу", " книгу", russian ), std::string( " книгу" ) );
		test::expectEqual( from( "книгу, да", ", да", russian ), std::string( ", да" ) );
		test::expectEqual( from( "日本語を勉強", "語を", japanese() ), std::string( "語を勉強" ) );
		test::expectEqual( from( "학교에서 사람을", "서 사", *lg::lang::findLanguage( "ko" ) ), std::string( "학교에서 사람을" ) );
		test::expectEqual( from( "в νότια", "τια", *lg::lang::findLanguage( "el" ) ), std::string( "νότια" ) );
		// At most `limit` characters back, and never from the middle of a character.
		test::expectEqual( lg::lang::wordStart( "книга", 8, russian, 2 ), std::size_t{ 4 } );
		test::expectEqual( lg::lang::wordStart( "книга", 3, russian ), std::size_t{ 3 } );
		test::expectEqual( lg::lang::wordStart( "книга", 99, russian ), std::size_t{ 99 } );
	} );

	const test::Registrar text_variants( "text variants", [] {
		std::vector<lg::lang::TextVariant> variants;
		japanese().variants( U"ｶﾞｯｺｳ", variants );
		const auto has = [&]( std::string_view text ) { return std::ranges::any_of( variants, [&]( const auto& v ) { return v.text == text; } ); };
		test::expect( has( "ガッコウ" ) && has( "がっこう" ) );
		test::expectEqual( variants.front().source_length.back(), std::uint32_t{ 5 } );

		variants.clear();
		japanese().variants( U"すっっごい", variants );
		test::expect( std::ranges::any_of( variants, []( const auto& v ) { return v.text == "すっごい" && v.source_length.back() == 5; } ) );
	} );

	const test::Registrar furigana( "furigana distribution", [] {
		const auto segments = lg::lang::ja::distributeFurigana( "食べ物", "たべもの" );
		test::expect( segments.size() == 3 && segments[0].reading == "た" && segments[1].reading.empty() && segments[2].reading == "もの" );

		const auto okurigana = lg::lang::ja::distributeFurigana( "取り扱い", "とりあつかい" );
		test::expect( okurigana.size() == 4 && okurigana[2].text == "扱" && okurigana[2].reading == "あつか" );

		const auto kana_only = lg::lang::ja::distributeFurigana( "コーヒー", "コーヒー" );
		test::expect( kana_only.size() == 1 && kana_only[0].reading.empty() );
	} );

	std::shared_ptr<const lg::lookup::DictionarySet> fixtureSet()
	{
		const auto output = test::scratch( "translator" ) / "fixture.lgd";
		if ( !lg::dict::compile( test::fixtureDirectory(), output ) )
		{
			return nullptr;
		}
		auto dictionary = lg::dict::Dictionary::open( output );
		if ( !dictionary )
		{
			return nullptr;
		}
		std::vector<lg::lookup::LoadedDictionary> loaded;
		loaded.push_back( { .dictionary = *dictionary, .styles = nullptr, .name = "Fixture" } );
		return std::make_shared<const lg::lookup::DictionarySet>( std::move( loaded ) );
	}

	const test::Registrar translator_lookups( "translator lookups", [] {
		const auto set = fixtureSet();
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;

		auto result = translator.lookup( set, "食べさせられなかったから" );
		if ( test::expect( !result.terms.empty() ) )
		{
			test::expectEqual( result.terms.front().expression, std::string_view( "食べる" ) );
			test::expectEqual( result.matched_length, std::uint32_t{ 10 } );
			test::expectEqual( result.inflectionText( result.terms.front() ), std::string( "causative « potential or passive « negative « past" ) );
			test::expect( !result.terms.front().frequencies.empty() && result.terms.front().frequencies.front().value == 199 );
			test::expect( !result.terms.front().pitches.empty() && result.terms.front().pitches.front().position == 2 );
		}

		result = translator.lookup( set, "日本語を勉強していました" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "日本語" && result.matched_length == 3 );
		test::expect( std::ranges::any_of( result.terms, []( const auto& t ) { return t.expression == "日本"; } ) );
		test::expect( !result.kanji.empty() && !result.kanji.front().frequencies.empty() );

		result = translator.lookup( set, "勉強していました" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "勉強" && result.matched_length == 8 );

		result = translator.lookup( set, "こーひーを飲む" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "コーヒー" && result.matched_length == 4 );

		result = translator.lookup( set, "ｶﾞｯｺｳ" );
		test::expect( !result.terms.empty() && result.terms.front().expression == "学校" && result.matched_length == 5 );

		result = translator.lookup( set, "hello" );
		test::expect( result.terms.empty() );

		result = translator.lookup( set, "。食べる" );
		test::expect( result.empty() );
	} );

	const test::Registrar selection_readings( "furigana over a selection, word by word", [] {
		const auto set = fixtureSet();
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		const auto             joined = []( const std::vector<lg::lang::RubySegment>& segments ) {
			std::string out;
			for ( const auto& segment : segments )
			{
				out += segment.reading.empty() ? segment.text : std::format( "{}[{}]", segment.text, segment.reading );
			}
			return out;
		};
		// Compounds, particles in between, and the kanji of inflected forms keep their readings.
		test::expectEqual( joined( translator.readings( set, "日本語を勉強していました。", {} ) ), std::string( "日本語[にほんご]を勉強[べんきょう]していました。" ) );
		test::expectEqual( joined( translator.readings( set, "食べさせられなかった", {} ) ), std::string( "食[た]べさせられなかった" ) );
		// What no dictionary has stays as it is.
		test::expectEqual( joined( translator.readings( set, "hello 学校", {} ) ), std::string( "hello 学校[がっこう]" ) );
	} );

	const test::Registrar translator_limit_checks( "translator limits", [] {
		const auto set = fixtureSet();
		if ( !test::expect( set != nullptr ) )
		{
			return;
		}
		lg::lookup::Translator translator;
		const auto             result = translator.lookup( set, "食べる", { .max_length = 1, .max_results = 1, .search_kanji = false } );
		test::expect( result.terms.empty() && result.text == "食" );

		const auto limited = translator.lookup( set, "高くない", { .max_length = 16, .max_results = 1, .search_kanji = false } );
		test::expectEqual( limited.terms.size(), std::size_t{ 1 } );
	} );

	const test::Registrar first_dictionaries( "first dictionaries only", [] {
		const auto sample = [] {
			lg::lookup::LookupResult result;
			result.terms.push_back( { .definitions = { { .dictionary = 2, .term = 0 }, { .dictionary = 0, .term = 1 } } } );
			result.terms.push_back( { .definitions = { { .dictionary = 1, .term = 2 } } } );
			result.terms.push_back( { .definitions = { { .dictionary = 3, .term = 3 } } } );
			return result;
		};
		auto all = sample();
		lg::lookup::keepFirstDictionaries( all, 0 );
		test::expectEqual( all.terms.size(), std::size_t{ 3 } );

		// Only the dictionary first in the priority list that has anything; entries left without definitions go.
		auto first = sample();
		lg::lookup::keepFirstDictionaries( first, 1 );
		test::expectEqual( first.terms.size(), std::size_t{ 1 } );
		if ( !first.terms.empty() )
		{
			test::expectEqual( first.terms.front().definitions.size(), std::size_t{ 1 } );
			test::expectEqual( static_cast<int>( first.terms.front().best_dictionary ), 0 );
		}

		auto two = sample();
		lg::lookup::keepFirstDictionaries( two, 2 );
		test::expectEqual( two.terms.size(), std::size_t{ 2 } );

		// The dictionary with the longest match comes first, whatever its priority: it has the word being looked at.
		lg::lookup::LookupResult longer;
		longer.terms.push_back( { .matched_length = 2, .definitions = { { .dictionary = 0, .term = 0 } } } );
		longer.terms.push_back( { .matched_length = 5, .definitions = { { .dictionary = 4, .term = 1 } } } );
		lg::lookup::keepFirstDictionaries( longer, 1 );
		test::expectEqual( longer.terms.size(), std::size_t{ 1 } );
		if ( !longer.terms.empty() )
		{
			test::expectEqual( static_cast<int>( longer.terms.front().best_dictionary ), 4 );
		}
	} );

} // namespace
