#include "Test.h"

#include <lexiglance/dictionary/Dictionary.h>
#include <lexiglance/dictionary/DictionaryStore.h>
#include <lexiglance/dictionary/Importer.h>
#include <lexiglance/dictionary/StructuredContent.h>
#include <lexiglance/dictionary/WordRules.h>

#include <algorithm>
#include <fstream>

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;
	using lg::dict::format::GlossKind;
	using lg::dict::format::MetaKind;

	std::shared_ptr<const lg::dict::Dictionary> compiled( const std::filesystem::path& source, std::string_view name )
	{
		const auto output  = test::scratch( name ) / "fixture.lgd";
		const auto summary = lg::dict::compile( source, output );
		if ( !test::expect( summary.has_value() ) )
		{
			return nullptr;
		}
		test::expectEqual( summary->title, std::string( "Fixture Dictionary [2026-01-01]" ) );
		test::expectEqual( summary->terms, std::uint64_t{ 11 } );
		auto dictionary = lg::dict::Dictionary::open( output );
		return dictionary ? *dictionary : nullptr;
	}

	bool containsTerm( const lg::dict::Dictionary& d, std::string_view key, std::string_view expression )
	{
		return std::ranges::any_of( d.findTerms( key ), [&]( std::uint32_t index ) { return d.string( d.terms()[index].expression ) == expression; } );
	}

	const test::Registrar compile_directory( "compile extracted dictionary", [] {
		const auto dictionary = compiled( test::fixtureDirectory(), "directory" );
		if ( !test::expect( dictionary != nullptr ) )
		{
			return;
		}
		const auto& d = *dictionary;
		test::expectEqual( d.info().title, std::string( "Fixture Dictionary [2026-01-01]" ) );
		test::expectEqual( d.info().revision, std::string( "fixture.1" ) );
		test::expect( d.info().updatable );

		test::expect( containsTerm( d, "食べる", "食べる" ) );
		test::expect( containsTerm( d, "たべる", "食べる" ) );
		test::expect( d.findTerms( "存在しない" ).empty() );

		const auto  index = d.findTerms( "食べる" ).front();
		const auto& term  = d.terms()[index];
		test::expect( ( term.rules & lg::dict::rule::v1 ) != 0 );
		test::expectEqual( d.string( term.term_tags ), std::string_view( "★" ) );
		const auto glossary = d.glossary( term );
		test::expect( glossary.size() == 2 && glossary[0].kind == GlossKind::Text && glossary[1].kind == GlossKind::StructuredContent );

		// Suru nouns are recognised from their part of speech tags.
		const auto study = d.findTerms( "勉強" );
		test::expect( !study.empty() && ( d.terms()[study.front()].rules & lg::dict::rule::vs ) != 0 );
	} );

	const test::Registrar compile_zip( "compile zip archive", [] {
		const auto dictionary = compiled( test::fixtureZip(), "zip" );
		test::expect( dictionary != nullptr && containsTerm( *dictionary, "コーヒー", "コーヒー" ) );
	} );

	const test::Registrar meta_and_kanji( "meta, kanji and tags", [] {
		const auto dictionary = compiled( test::fixtureDirectory(), "meta" );
		if ( !test::expect( dictionary != nullptr ) )
		{
			return;
		}
		const auto& d = *dictionary;

		int frequencies = 0;
		int pitches     = 0;
		for ( const auto index : d.findMeta( "食べる" ) )
		{
			const auto& meta = d.meta()[index];
			frequencies += meta.kind == MetaKind::Frequency && meta.value == 199 ? 1 : 0;
			pitches += meta.kind == MetaKind::Pitch && meta.value == 2 ? 1 : 0;
		}
		test::expectEqual( frequencies, 1 );
		test::expectEqual( pitches, 1 );

		const auto* kanji = d.findKanji( U'日' );
		if ( test::expect( kanji != nullptr ) )
		{
			test::expectEqual( d.string( kanji->onyomi ), std::string_view( "ニチ ジツ" ) );
			test::expectEqual( kanji->meanings_count, std::uint32_t{ 3 } );
			test::expectEqual( kanji->stats_count, std::uint32_t{ 4 } );
		}
		test::expect( d.findKanji( U'月' ) == nullptr );

		const auto* tag = d.findTag( "v1" );
		test::expect( tag != nullptr && d.string( tag->category ) == "partOfSpeech" );
		test::expect( d.findTag( "missing" ) == nullptr );
		test::expect( d.styles().contains( "data-sc-content" ) );
	} );

	const test::Registrar store_management( "dictionary store", [] {
		const lg::dict::DictionaryStore store( test::scratch( "store" ) );
		test::expect( store.install( test::fixtureDirectory(), {}, false ).has_value() );
		test::expect( !store.install( test::fixtureDirectory(), {}, false ).has_value() );
		test::expect( store.install( test::fixtureDirectory(), {}, true ).has_value() );
		test::expectEqual( store.loadAll().size(), std::size_t{ 1 } );
		test::expect( store.fileFor( "Fixture Dictionary [2026-01-01]" ).filename().string().starts_with( "fixture-dictionary-2026-01-01-" ) );
		test::expect( store.remove( "Fixture Dictionary [2026-01-01]" ).has_value() );
		test::expect( store.loadAll().empty() );
	} );

	void corruptFiles()
	{
		const auto directory = test::scratch( "corrupt" );
		{
			std::ofstream out( directory / "broken.lgd", std::ios::binary );
			out << "not a dictionary at all, just some bytes that are long enough to look like a header maybe";
		}
		test::expect( !lg::dict::Dictionary::open( directory / "broken.lgd" ).has_value() );
		test::expect( !lg::dict::compile( directory, directory / "out.lgd" ).has_value() );
	}

	const test::Registrar corrupt_files( "rejects corrupt files", &corruptFiles );

	const test::Registrar structured_content( "structured content rendering", [] {
		constexpr std::string_view content =
				R"({"tag":"ul","content":[{"tag":"li","content":"to live on"},{"tag":"li","content":{"tag":"span","data":{"content":"note"},"content":"figurative"}}]})";

		std::string plain;
		lg::dict::renderGlossary( plain, GlossKind::StructuredContent, content, { .format = lg::dict::Markup::Plain } );
		test::expectEqual( plain, std::string( "• to live on\n• figurative" ) );

		const auto styles =
				lg::dict::StyleSheet::parse( R"(span[data-sc-content="note"] { color: #ff0000; font-weight: bold; } div[data-sc-content="x"] { & ul { color: red; } })" );
		std::string pango;
		lg::dict::renderGlossary( pango, GlossKind::StructuredContent, content, { .format = lg::dict::Markup::Pango, .styles = &styles } );
		test::expect( pango.contains( "foreground=\"#ff0000\"" ) && pango.contains( "weight=\"bold\"" ) );

		std::string html;
		lg::dict::renderGlossary( html, GlossKind::Text, "a < b & \"c\"", { .format = lg::dict::Markup::Html } );
		test::expectEqual( html, std::string( "a &lt; b &amp; &quot;c&quot;" ) );

		std::string tags;
		lg::dict::renderGlossary( tags, GlossKind::StructuredContent, R"([{"tag":"span","data":{"class":"tag"},"content":"n"},{"tag":"span","data":{"class":"tag"},"content":"vt"}])", { .format = lg::dict::Markup::Html } );
		test::expect( tags.contains( "&nbsp;n&nbsp;</span> " ) && tags.contains( "&nbsp;vt&nbsp;</span> " ) );

		std::string ruby;
		lg::dict::renderGlossary( ruby, GlossKind::StructuredContent, R"({"tag":"ruby","content":["食",{"tag":"rt","content":"た"}]})", { .format = lg::dict::Markup::Html } );
		test::expect( ruby.contains( "(た)" ) && !ruby.contains( "<rt" ) );

		// Jitendex: numbered senses, each a list of glosses.
		std::string nested;
		lg::dict::renderGlossary( nested, GlossKind::StructuredContent, R"({"tag":"ol","content":{"tag":"li","content":{"tag":"ul","content":[{"tag":"li","content":"to eat"},{"tag":"li","content":"to live on"}]}}})", { .format = lg::dict::Markup::Plain } );
		test::expectEqual( nested, std::string( "1. • to eat\n\u2003• to live on" ) );

		// Wiktionary: grammar and examples in collapsed sections, which only the settings application shows.
		constexpr std::string_view sections =
				R"([{"tag":"details","content":[{"tag":"summary","content":"Grammar"},"f inan"]},"book",{"tag":"details","open":true,"content":[{"tag":"summary","content":"Usage"},"common"]}])";
		std::string closed;
		lg::dict::renderGlossary( closed, GlossKind::StructuredContent, sections, { .format = lg::dict::Markup::Plain } );
		test::expectEqual( closed, std::string( "book\nUsage\ncommon" ) );
		std::string all;
		lg::dict::renderGlossary( all, GlossKind::StructuredContent, sections, { .format = lg::dict::Markup::Html } );
		test::expect( all.contains( "Grammar" ) && all.contains( "f inan" ) );
	} );

} // namespace
