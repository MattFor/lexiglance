#include "Test.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Glob.h>
#include <lexiglance/core/Hash.h>
#include <lexiglance/core/Md5.h>
#include <lexiglance/core/Process.h>
#include <lexiglance/core/Thread.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/core/Zip.h>
#include <lexiglance/ipc/Protocol.h>

#include <fstream>
#include <sstream>

#ifndef _WIN32
	#include <unistd.h>
#endif

namespace
{

	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	static_assert( lg::hash64( "a" ) != lg::hash64( "b" ) );
	static_assert( lg::utf8::length( "日本語" ) == 3 );
	static_assert( lg::globMatch( "steam_app_*", "steam_app_1234.Steam_app_1234" ) );

	const test::Registrar utf8_decoding( "utf8 decoding", [] {
		constexpr std::string_view text = "aé日😀";
		test::expectEqual( lg::utf8::length( text ), std::size_t{ 4 } );
		test::expectEqual( lg::utf8::first( text ), U'a' );
		test::expectEqual( lg::utf8::last( text ), U'😀' );
		test::expectEqual( lg::utf8::prefix( text, 3 ), std::string_view( "aé日" ) );
		test::expectEqual( lg::utf8::fromUtf32( lg::utf8::toUtf32( text ) ), std::string( text ) );

		std::size_t pos = 0;
		test::expectEqual( lg::utf8::decode( "\xC0\x80", pos ), lg::utf8::replacement_character );
		test::expectEqual( pos, std::size_t{ 1 } );
		pos = 0;
		test::expectEqual( lg::utf8::decode( "\xED\xA0\x80", pos ), lg::utf8::replacement_character );
	} );

	const test::Registrar utf8_iteration( "utf8 iteration", [] {
		std::u32string out;
		for ( const char32_t c : lg::utf8::codepoints( "かなカナ" ) )
		{
			out.push_back( c );
		}
		test::expectEqual( out.size(), std::size_t{ 4 } );
		test::expect( out == U"かなカナ" );
	} );

	const test::Registrar hashing( "hash stability", [] {
		test::expectEqual( lg::hash64( "食べる" ), lg::hash64( std::string( "食べる" ) ) );
		test::expect( lg::hash64( "abcdefgh12345678" ) != lg::hash64( "abcdefgh12345679" ) );
		test::expect( lg::hash64( "" ) != 0 );
	} );

	const test::Registrar json_parsing( "json parsing", [] {
		auto document = lg::json::Document::parse( R"({"a":[1,2.5,-3e2,true,null],"s":"\u3042\ud83d\ude00\n\"x\"","o":{"k":"v"}})" );
		if ( !test::expect( document.has_value() ) )
		{
			return;
		}
		const auto& root = document->root();
		test::expectEqual( root["a"].size(), std::size_t{ 5 } );
		test::expectEqual( root["a"][1].asDouble(), 2.5 );
		test::expectEqual( root["a"][2].asInt(), std::int64_t{ -300 } );
		test::expect( root["a"][3].asBool() );
		test::expect( root["a"][4].isNull() );
		test::expectEqual( root["s"].asString(), std::string_view( "あ😀\n\"x\"" ) );
		test::expectEqual( root["o"]["k"].asString(), std::string_view( "v" ) );
		test::expect( root["missing"]["deep"].isNull() );
	} );

	const test::Registrar json_errors( "json errors", [] {
		test::expect( !lg::json::Document::parse( "[1,2" ).has_value() );
		test::expect( !lg::json::Document::parse( R"({"a":})" ).has_value() );
		test::expect( !lg::json::Document::parse( R"("\x")" ).has_value() );
		test::expect( !lg::json::Document::parse( "[1] 2" ).has_value() );
		const auto error = lg::json::Document::parse( "{\n\"a\": tru}" );
		test::expect( !error && error.error().message.contains( "line 2" ) );
	} );

	const test::Registrar json_round_trip( "json round trip", [] {
		constexpr std::string_view source = R"({"tag":"span","style":{"fontSize":"80%"},"content":["a","\"b\"",1,false]})";
		auto                       first  = lg::json::Document::parse( std::string( source ) );
		if ( !test::expect( first.has_value() ) )
		{
			return;
		}
		const auto serialized = lg::json::serialize( first->root() );
		test::expectEqual( serialized, std::string( source ) );

		lg::json::Writer writer;
		writer.beginObject().field( "n", 42 ).field( "s", "é\t" ).key( "list" ).beginArray().value( true ).null().endArray().endObject();
		test::expectEqual( writer.str(), std::string( R"({"n":42,"s":"é\t","list":[true,null]})" ) );
	} );

	const test::Registrar glob_matching( "glob matching", [] {
		test::expect( lg::globMatch( "*", "" ) );
		test::expect( lg::globMatch( "wine*", "Wine.exe" ) );
		test::expect( lg::globMatch( "?at", "cat" ) );
		test::expect( !lg::globMatch( "?at", "at" ) );
		test::expect( lg::globMatch( "*.Firefox", "Navigator.firefox" ) );
		test::expect( !lg::globMatch( "steam", "steam_app_1" ) );
	} );

	void zipReading()
	{
		auto archive = lg::ZipArchive::open( test::fixtureZip() );
		if ( !test::expect( archive.has_value() ) )
		{
			return;
		}
		const auto* index = archive->find( "index.json" );
		if ( !test::expect( index != nullptr ) )
		{
			return;
		}
		const auto          content = archive->read( *index );
		const std::ifstream original( test::fixtureDirectory() / "index.json", std::ios::binary );
		std::stringstream   buffer;
		buffer << original.rdbuf();
		test::expect( content.has_value() && *content == buffer.str() );
		test::expect( archive->find( "missing.json" ) == nullptr );
	}

	const test::Registrar zip_reading( "zip reading", &zipReading );

	const test::Registrar config_round_trip( "config round trip", [] {
		lg::config::Config config;
		config.scan.trigger   = { "Super", "Alt_L" };
		config.popup.width    = 512;
		config.popup.theme    = lg::config::Theme::Light;
		config.scan.selection = lg::config::SelectionMode::WithTrigger;
		config.dictionaries   = { { .title = "JMdict [2026-09-12]", .enabled = true }, { .title = "Jiten", .enabled = false } };

		const auto parsed = lg::config::Config::parse( config.toJson() );
		if ( !test::expect( parsed.has_value() ) )
		{
			return;
		}
		test::expect( parsed->scan.trigger == config.scan.trigger );
		test::expectEqual( parsed->popup.width, 512 );
		test::expect( parsed->popup.theme == lg::config::Theme::Light );
		test::expect( parsed->scan.selection == lg::config::SelectionMode::WithTrigger );
		test::expectEqual( parsed->dictionaries.size(), std::size_t{ 2 } );
		test::expect( parsed->dictionary( "Jiten" ) != nullptr && !parsed->dictionary( "Jiten" )->enabled );
	} );

	const test::Registrar config_validation( "config validation", [] {
		test::expect( !lg::config::Config::parse( R"({"scan":{"trigger":["Hyper"]}})" ).has_value() );
		const auto clamped = lg::config::Config::parse( R"({"popup":{"width":5,"font_size":999}})" );
		test::expect( clamped && clamped->popup.width == 200 && clamped->popup.font_size == 72 );
		test::expect( lg::config::Config::parse( "{}" ).has_value() );
	} );

	const test::Registrar key_names( "key names", [] {
		const auto super = lg::config::parseKey( "super" );
		test::expect( super && super->size() == 2 );
		const auto chord = lg::config::parseChord( std::vector<std::string>{ "Win", "LAlt" } );
		test::expect( chord && chord->size() == 2 && ( *chord )[1] == lg::config::KeyGroup{ lg::config::Key::AltL } );
		test::expectEqual( lg::config::keyName( lg::config::Key::SuperL ), std::string_view( "Super_L" ) );
		test::expect( lg::config::isMouseButton( lg::config::Key::MouseBack ) );

		// Both spellings of the Windows key parse everywhere, so a configuration written on either platform loads on
		// the other; only what the user is shown follows the platform.
		for ( const char* name : { "Win_L", "LWin", "Super_L" } )
		{
			const auto left = lg::config::parseKey( name );
			test::expect( left && *left == lg::config::KeyGroup{ lg::config::Key::SuperL } );
		}
		const auto right = lg::config::parseKey( "RWin" );
		test::expect( right && *right == lg::config::KeyGroup{ lg::config::Key::SuperR } );
#ifdef _WIN32
		test::expectEqual( lg::config::displayKeyName( lg::config::Key::SuperL ), std::string_view( "Left Win" ) );
		test::expectEqual( lg::config::displayName( "Super" ), std::string( "Win" ) );
		test::expectEqual( lg::config::displayName( "Meta" ), std::string( "Win" ) );
		test::expectEqual( lg::config::displayName( "Super_R" ), std::string( "Right Win" ) );
#else
		test::expectEqual( lg::config::displayKeyName( lg::config::Key::SuperL ), std::string_view( "Left Super" ) );
		test::expectEqual( lg::config::displayName( "Super" ), std::string( "Super" ) );
#endif
		test::expectEqual( lg::config::displayName( "Alt_L" ), std::string( "Left Alt" ) );
		test::expectEqual( lg::config::displayName( "Control_R" ), std::string( "Right Ctrl" ) );
		test::expectEqual( lg::config::displayName( "Caps_Lock" ), std::string( "Caps Lock" ) );
		test::expectEqual( lg::config::displayName( "Hyper" ), std::string( "Hyper" ) );
	} );

	const test::Registrar ipc_framing( "ipc framing", [] {
		lg::ipc::LineBuffer buffer;
		buffer.append( lg::ipc::request( 7, "status" ) );
		buffer.append( "{\"partial\":" );
		const auto line = buffer.next();
		test::expect( line.has_value() && line->contains( R"("method":"status")" ) );
		test::expect( !buffer.next().has_value() );
		buffer.append( "1}\n" );
		test::expectEqual( buffer.next().value_or( "" ), std::string( "{\"partial\":1}" ) );
	} );

	const test::Registrar md5_vectors( "md5", [] {
		test::expectEqual( lg::md5Hex( "" ), std::string( "d41d8cd98f00b204e9800998ecf8427e" ) );
		test::expectEqual( lg::md5Hex( "abc" ), std::string( "900150983cd24fb0d6963f7d28e17f72" ) );
		test::expectEqual( lg::md5Hex( "The quick brown fox jumps over the lazy dog" ), std::string( "9e107d9d372bb6826bd81d3542a419d6" ) );
	} );

	const test::Registrar config_newer_settings( "configuration round trip of audio, anki and highlight settings", [] {
		lg::config::Config config;
		config.popup.highlight_style     = lg::config::HighlightStyle::Fill;
		config.popup.highlight_thickness = 5;
		config.popup.highlight_auto      = true;
		config.popup.select_button       = lg::config::MouseButton::Middle;
		config.audio.autoplay            = true;
		config.audio.sources             = { "jpod101", "http://localhost:5050/?term={term}&reading={reading}" };
		config.anki.enabled              = true;
		config.anki.deck                 = "Mining";
		config.anki.fields               = { { .name = "Front", .value = "{expression}" }, { .name = "Back", .value = "{glossary}" } };

		const auto parsed = lg::config::Config::parse( config.toJson() );
		test::expect( parsed.has_value() );
		if ( !parsed )
		{
			return;
		}
		test::expect( parsed->popup.highlight_style == lg::config::HighlightStyle::Fill );
		test::expectEqual( parsed->popup.highlight_thickness, 5 );
		test::expect( parsed->popup.highlight_auto );
		test::expect( parsed->popup.select_button == lg::config::MouseButton::Middle );
		test::expect( parsed->audio.autoplay );
		test::expectEqual( parsed->audio.sources.size(), std::size_t{ 2 } );
		test::expect( parsed->anki.enabled );
		test::expectEqual( parsed->anki.deck, std::string( "Mining" ) );
		test::expectEqual( parsed->anki.fields.size(), std::size_t{ 2 } );
		test::expectEqual( parsed->anki.fields.back().value, std::string( "{glossary}" ) );
	} );
	void                  zip64ExtraBounds()
	{
		// One stored entry whose central header claims a zip64 size in an extra field that is cut short; the 8 bytes
		// after it (the entry comment) must not be taken for the size.
		std::string bytes;
		const auto  put = [&]( std::uint64_t value, int size ) {
			for ( int i = 0; i < size; ++i )
			{
				bytes.push_back( static_cast<char>( ( value >> ( 8 * i ) ) & 0xFFU ) );
			}
		};
		put( 0x04034b50, 4 );
		put( 20, 2 );
		put( 0, 2 );
		put( 0, 2 );
		put( 0, 4 );
		put( 0, 4 );
		put( 2, 4 );
		put( 2, 4 );
		put( 1, 2 );
		put( 0, 2 );
		bytes += "ahi";
		const auto directory = bytes.size();
		put( 0x02014b50, 4 );
		put( 20, 2 );
		put( 20, 2 );
		put( 0, 2 );
		put( 0, 2 );
		put( 0, 4 );
		put( 0, 4 );
		put( 2, 4 );
		put( 0xFFFFFFFF, 4 );
		put( 1, 2 );
		put( 4, 2 );
		put( 8, 2 );
		put( 0, 2 );
		put( 0, 2 );
		put( 0, 4 );
		put( 0, 4 );
		bytes += "a";
		put( 0x0001, 2 );
		put( 0xFFFF, 2 );
		put( 2, 8 );
		const auto directory_size = bytes.size() - directory;
		put( 0x06054b50, 4 );
		put( 0, 2 );
		put( 0, 2 );
		put( 1, 2 );
		put( 1, 2 );
		put( directory_size, 4 );
		put( directory, 4 );
		put( 0, 2 );

		const auto path = std::filesystem::temp_directory_path() / "lexiglance-test-zip64.zip";
		{
			std::ofstream out( path, std::ios::binary | std::ios::trunc );
			out.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
		}
		bool          read = false;
		std::uint64_t size = 0;
		{
			// Closed before the file goes: Windows cannot delete a file that is still mapped.
			const auto archive = lg::ZipArchive::open( path );
			read               = archive.has_value() && archive->entries().size() == 1;
			size               = read ? archive->entries().front().size : 0;
		}
		std::filesystem::remove( path );
		if ( !test::expect( read ) )
		{
			return;
		}
		test::expectEqual( size, std::uint64_t{ 0xFFFFFFFF } );
	}

	const test::Registrar zip64_extra_bounds( "zip64 extra field stays inside its entry", &zip64ExtraBounds );

	const test::Registrar config_languages( "configuration of languages, and a setting's earlier name", [] {
		lg::config::Config config;
		config.disabled_languages = { "ja" };
		const auto parsed         = lg::config::Config::parse( config.toJson() );
		test::expect( parsed && parsed->disabled_languages == std::vector<std::string>{ "ja" } );
		const auto old = lg::config::Config::parse( R"({"scan":{"japanese_only":false}})" );
		test::expect( old && !old->scan.known_languages_only );
		const lg::config::Config defaults;
		test::expect( defaults.scan.known_languages_only && defaults.disabled_languages.empty() && defaults.audio.sources == std::vector<std::string>{ "jpod101", "commons" } );
	} );

#ifdef __linux__
	const test::Registrar process_name( "naming the main thread leaves the process known by its program's name", [] {
		const std::string before( lg::thread::name() );
		lg::thread::setName( "renamed" );
		test::expectEqual( lg::thread::name(), std::string_view( "renamed" ) );
		// ps, pgrep and othersNamed() still see lexiglance_tests (the kernel keeps 15 characters of it).
		test::expect( lg::process::isRunning( ::getpid(), "lexiglance_tests" ) );
		test::expect( !lg::process::isRunning( ::getpid(), "renamed" ) );
		lg::thread::setName( before );
	} );
#endif

	const test::Registrar config_translation( "configuration of translation, languages whose translation is off included", [] {
		lg::config::Config config;
		config.translation.selections         = false;
		config.translation.sentence_key       = "Control_R";
		config.translation.disabled_languages = { "ru", "el" };
		config.translation.model              = "full";
		config.translation.setModelFor( "ja", "compact" );
		const auto parsed = lg::config::Config::parse( config.toJson() );
		if ( !test::expect( parsed.has_value() ) )
		{
			return;
		}
		test::expect( !parsed->translation.selections );
		test::expectEqual( parsed->translation.sentence_key, std::string( "Control_R" ) );
		test::expect( parsed->translation.disabled_languages == std::vector<std::string>{ "ru", "el" } );
		test::expect( !parsed->translation.translates( "ru" ) && parsed->translation.translates( "ja" ) );
		test::expectEqual( parsed->translation.model, std::string( "full" ) );
		// A language with weights of its own keeps them; the others follow the one above the table.
		test::expectEqual( parsed->translation.modelFor( "ja" ), std::string_view( "compact" ) );
		test::expectEqual( parsed->translation.modelFor( "ru" ), std::string_view( "full" ) );
		// Chosen again: the one entry changes rather than a second one appearing.
		auto again = *parsed;
		again.translation.setModelFor( "ja", "full" );
		test::expect( again.translation.models.size() == 1 && again.translation.modelFor( "ja" ) == "full" );
		// Anything but "full" in the file means the compact weights.
		const auto odd_weights = lg::config::Config::parse( R"({"translation":{"models":{"ja":"huge","ru":"full"}}})" );
		test::expect( odd_weights && odd_weights->translation.modelFor( "ja" ) == "compact" && odd_weights->translation.modelFor( "ru" ) == "full" );
		// Every language is translated to begin with, and a file from before has none turned off and compact models.
		const auto old = lg::config::Config::parse( R"({"translation":{"enabled":true}})" );
		test::expect( old && old->translation.disabled_languages.empty() && old->translation.translates( "uk" ) && old->translation.model == "compact" );
		// Anything else is compact too, as are the statistics of translations counted to begin with.
		const auto odd = lg::config::Config::parse( R"({"translation":{"model":"huge"},"statistics":false})" );
		test::expect( odd && odd->translation.model == "compact" && !odd->statistics && odd->statistics_translations );
		const auto split = lg::config::Config::parse( R"({"statistics_translations":false})" );
		test::expect( split && split->statistics && !split->statistics_translations );
	} );

	const test::Registrar config_newest_settings( "configuration round trip of compositor, wheel and dictionary settings", [] {
		lg::config::Config config;
		config.popup.compositor          = lg::config::Compositor::Off;
		config.popup.max_dictionaries    = 3;
		config.scan.wheel_length         = false;
		config.scan.wheel_lock           = true;
		config.scan.known_languages_only = false;
		config.scan.ocr_engine           = lg::config::OcrEngine::Tesseract;

		const auto parsed = lg::config::Config::parse( config.toJson() );
		if ( !test::expect( parsed.has_value() ) )
		{
			return;
		}
		test::expect( parsed->popup.compositor == lg::config::Compositor::Off );
		test::expectEqual( parsed->popup.max_dictionaries, 3 );
		test::expect( !parsed->scan.wheel_length );
		test::expect( parsed->scan.wheel_lock );
		test::expect( !parsed->scan.known_languages_only );
		test::expect( parsed->scan.ocr_engine == lg::config::OcrEngine::Tesseract );
		// Out-of-range and unknown values fall back to safe ones.
		const auto odd = lg::config::Config::parse( R"({"popup":{"compositor":"sometimes","max_dictionaries":-4}})" );
		test::expect( odd && odd->popup.compositor == lg::config::Compositor::Auto && odd->popup.max_dictionaries == 0 );
	} );

	const test::Registrar ipc_overflow( "ipc line buffer refuses endless lines", [] {
		lg::ipc::LineBuffer buffer;
		const std::string   chunk( std::size_t{ 64 } * 1024, 'x' );
		for ( int i = 0; i < 1100 && !buffer.overflowed(); ++i )
		{
			buffer.append( chunk );
			if ( !test::expect( !buffer.next().has_value() ) )
			{
				return;
			}
		}
		test::expect( buffer.overflowed() );
	} );

	void zipBomb()
	{
		// Found by fuzzing: 26 compressed bytes declaring nearly 4 GiB. Reading must fail before allocating anything.
		const auto archive = lg::ZipArchive::open( test::fixtureDirectory().parent_path() / "regressions" / "zip-bomb.zip" );
		if ( !test::expect( archive.has_value() ) )
		{
			return;
		}
		const auto* bomb = archive->find( "kanji_meta_bank_1.json" );
		if ( !test::expect( bomb != nullptr ) )
		{
			return;
		}
		test::expect( !archive->read( *bomb ).has_value() );
		const auto* index = archive->find( "index.json" );
		test::expect( index != nullptr && archive->read( *index ).has_value() );
	}

	const test::Registrar zip_bomb( "zip entries declaring impossible sizes are refused", &zipBomb );

} // namespace
