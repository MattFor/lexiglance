#include "Test.h"

#include <lexiglance/dictionary/Classify.h>
#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/Importer.h>

#include <string>
#include <vector>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;
	using lg::dict::Kind;
	using lg::dict::Profile;

	const test::Registrar dictionary_kinds( "what a dictionary is for", [] {
		const auto kind = []( const Profile& profile ) { return lg::dict::classify( profile ); };
		test::expect( kind( { .title = "JMdict [2026-09-13]", .terms = 527058 } ) == Kind::Words );
		test::expect( kind( { .title = "JMnedict [2026-09-13]", .terms = 667965 } ) == Kind::Names );
		test::expect( kind( { .title = "Persons", .terms = 90000, .name_share = 0.9 } ) == Kind::Names );
		test::expect( kind( { .title = "日本語文型辞典", .terms = 1186 } ) == Kind::Grammar );
		test::expect( kind( { .title = "surasura 擬声語", .terms = 1422 } ) == Kind::Specialized );
		// Which sites have an article: large, but one definition repeated.
		test::expect( kind( { .title = "Nico/Pixiv", .terms = 92272, .distinct_share = 0.01 } ) == Kind::Specialized );
		test::expect( kind( { .title = "大辞林", .terms = 250000, .native_share = 0.9 } ) == Kind::Monolingual );
		test::expect( kind( { .title = "wty-ja-ja", .source_language = "ja", .target_language = "ja", .terms = 306136 } ) == Kind::Monolingual );
		// Jitendex quotes Japanese in its examples, yet it is the word dictionary.
		test::expect( kind( { .title = "Jitendex.org [2026-08-11]", .terms = 435448, .native_share = 0.7, .structured = true } ) == Kind::Words );
		test::expect( kind( { .title = "KANJIDIC", .kanji = 10384 } ) == Kind::Kanji );
		test::expect( kind( { .title = "JPDB", .frequencies = 338814 } ) == Kind::Frequency );
		test::expect( kind( { .title = "NHK", .pitches = 100000 } ) == Kind::Pitch );
		test::expect( kind( { .title = "Empty" } ) == Kind::Other );
		test::expectEqual( lg::dict::kindName( Kind::Names ), std::string_view( "Names" ) );
	} );

	const test::Registrar smart_order( "smart order of the priority list", [] {
		const Profile jitendex{ .title = "Jitendex.org [2026-08-11]", .terms = 435448, .structured = true };
		const Profile jmdict{ .title = "JMdict [2026-09-13]", .terms = 527058 };
		const Profile jmnedict{ .title = "JMnedict [2026-09-13]", .terms = 667965 };
		const Profile kanjidic{ .title = "KANJIDIC", .kanji = 10384 };
		const Profile bccwj{ .title = "BCCWJ", .frequencies = 1000219 };

		const std::vector<Profile> profiles{
			bccwj,
			jmnedict,
			kanjidic,
			jmdict,
			{ .title = "wty-ja-en", .terms = 338662, .structured = true },
			jitendex,
			{ .title = "日本語文型辞典", .terms = 1186 },
			{ .title = "JPDB", .frequencies = 338814 },
		};
		std::vector<std::string> titles;
		for ( const std::size_t index : lg::dict::smartOrder( profiles ) )
		{
			titles.push_back( profiles[index].title );
		}
		const std::vector<std::string> expected{
			"Jitendex.org [2026-08-11]",
			"JMdict [2026-09-13]",
			"wty-ja-en",
			"JMnedict [2026-09-13]",
			"日本語文型辞典",
			"KANJIDIC",
			"BCCWJ",
			"JPDB",
		};
		test::expect( titles == expected );

		// A newly installed dictionary goes where it belongs, not last; one of a kind already there goes after it.
		const std::vector<Profile> existing{ jitendex, jmnedict, kanjidic, bccwj };
		test::expectEqual( lg::dict::insertionPoint( existing, jmdict ), std::size_t{ 1 } );
		test::expectEqual( lg::dict::insertionPoint( existing, { .title = "JPDB Kanji", .kanji = 6494 } ), std::size_t{ 3 } );
		test::expectEqual( lg::dict::insertionPoint( existing, { .title = "NHK", .pitches = 1 } ), std::size_t{ 4 } );
	} );

	const test::Registrar fixture_profile( "profile of a compiled dictionary", [] {
		const auto output = test::scratch( "classify" ) / "fixture.lgd";
		if ( !test::expect( lg::dict::compile( test::fixtureDirectory(), output ).has_value() ) )
		{
			return;
		}
		const auto dictionary = lg::dict::Dictionary::open( output );
		if ( !test::expect( dictionary.has_value() ) )
		{
			return;
		}
		const auto profile = lg::dict::profile( **dictionary );
		test::expectEqual( profile.title, std::string( "Fixture Dictionary [2026-01-01]" ) );
		test::expectEqual( profile.terms, std::size_t{ 11 } );
		test::expect( profile.distinct_share > 0.5 );
		test::expect( profile.native_share < 0.5 );
		test::expect( profile.name_share < 0.5 );
		// Eleven words: a small word list.
		test::expect( lg::dict::classify( profile ) == Kind::Specialized );
	} );

} // namespace
