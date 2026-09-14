#include "Test.h"

#include "Anki.h"
#include "Audio.h"

#include <lexiglance/core/Md5.h>

#include <format>
#include <string>

namespace
{

	namespace test   = lexiglance::test;
	namespace daemon = lexiglance::daemon;

	const test::Registrar anki_templates( "anki field templates", [] {
		const daemon::Markers markers{ { "expression", "食べる" }, { "reading", "たべる" }, { "cloze-body", "食べた" } };
		test::expectEqual( daemon::fillTemplate( "{expression}[{reading}]", markers ), std::string( "食べる[たべる]" ) );
		test::expectEqual( daemon::fillTemplate( "<b>{cloze-body}</b>", markers ), std::string( "<b>食べた</b>" ) );
		// Unknown markers and unclosed braces stay as they are.
		test::expectEqual( daemon::fillTemplate( "{nope} {expression", markers ), std::string( "{nope} {expression" ) );
	} );

	const test::Registrar anki_sentence( "sentence around a lookup", [] {
		const std::string text    = "前の文。昨日は何も食べなかった！次の文";
		const auto        offset  = text.find( "食べ" );
		const auto [sentence, at] = daemon::sentenceAround( text, offset );
		test::expectEqual( sentence, std::string( "昨日は何も食べなかった！" ) );
		test::expectEqual( sentence.substr( at, 6 ), std::string( "食べ" ) );

		// Line breaks end sentences without being part of them.
		const auto [line, line_at] = daemon::sentenceAround( "一行目\n二行目の文", 10 );
		test::expectEqual( line, std::string( "二行目の文" ) );
		test::expectEqual( line_at, std::size_t{ 0 } );
	} );

	const test::Registrar audio_sources( "pronunciation sources by language", [] {
		const auto* russian  = lexiglance::lang::findLanguage( "ru" );
		const auto* japanese = lexiglance::lang::findLanguage( "ja" );
		// JapanesePod101 has Japanese words only; Wikimedia Commons names recordings by language, with an MP3 of each.
		test::expect( daemon::audioUrl( "jpod101", "книга", "", russian ).empty() );
		test::expect( daemon::audioUrl( "jpod101", "食べる", "たべる", japanese ).contains( "kana=" ) );
		const auto hash = lexiglance::md5Hex( "Ru-книга.ogg" );
		test::expectEqual(
				daemon::audioUrl( "commons", "книга", "кни́га", russian ),
				std::format( "https://upload.wikimedia.org/wikipedia/commons/transcoded/{}/{}/Ru-%D0%BA%D0%BD%D0%B8%D0%B3%D0%B0.ogg/Ru-%D0%BA%D0%BD%D0%B8%D0%B3%D0%B0.ogg.mp3", hash.substr( 0, 1 ), hash.substr( 0, 2 ) )
		);
		test::expectEqual( daemon::audioUrl( "http://localhost/{language}/{term}", "стол", "", russian ), std::string( "http://localhost/ru/%D1%81%D1%82%D0%BE%D0%BB" ) );
	} );

} // namespace
