#include "Test.h"

#include "Translation.h"

#include <lexiglance/language/Language.h>

#include <chrono>
#include <future>
#include <map>
#include <string>

namespace
{

	namespace lg     = lexiglance;
	namespace test   = lexiglance::test;
	namespace daemon = lexiglance::daemon;

	const test::Registrar translation_service( "translation service without a model", [] {
		// A language whose model is not downloaded (and never will be: no such repository).
		auto language = lg::lang::parseLanguage( R"({"code": "xx", "script": ["0400-04FF"], "translation": {"model": "Nobody/opus-mt-xx-en", "revision": "0"}})" );
		if ( !test::expect( language.has_value() ) )
		{
			return;
		}
		const auto& xx = **language;
		test::expect( daemon::TranslationService::unavailable( xx ).contains( "needs its model" ) );

		std::promise<std::string>  reported;
		auto                       answer = reported.get_future();
		daemon::TranslationService service( [&]( std::uint64_t ticket, lg::Result<std::string> translated, std::chrono::microseconds ) {
			reported.set_value( std::to_string( ticket ) + ( translated ? " " + *translated : " error: " + translated.error().message ) );
		} );
		// The request is answered on the translation thread, with why there is no translation.
		service.request( 7, xx, "книга" );
		if ( test::expect( answer.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready ) )
		{
			const std::string result = answer.get();
			test::expect( result.starts_with( "7 error: " ) && result.contains( "needs its model" ) );
		}
		test::expect( !service.cached( xx, "книга" ).has_value() );
		const auto now = service.translateNow( xx, "книга", std::chrono::seconds( 10 ) );
		test::expect( !now.has_value() );

		// No model named at all.
		auto none = lg::lang::parseLanguage( R"({"code": "yy", "script": ["0400-04FF"]})" );
		test::expect( none.has_value() && daemon::TranslationService::unavailable( **none ).contains( "has no translation model" ) );
	} );

	const test::Registrar translation_service_precisions( "each language is loaded from the weights it was given", [] {
		auto japanese = lg::lang::parseLanguage( R"({"code": "ja", "script": ["3040-309F"], "translation": {"model": "Nobody/opus-mt-ja-en", "revision": "0"}})" );
		auto russian  = lg::lang::parseLanguage( R"({"code": "ru", "script": ["0400-04FF"], "translation": {"model": "Nobody/opus-mt-ru-en", "revision": "0"}})" );
		if ( !test::expect( japanese.has_value() && russian.has_value() ) )
		{
			return;
		}
		daemon::TranslationService service( []( std::uint64_t, lg::Result<std::string>, std::chrono::microseconds ) {} );
		// Nothing chosen yet: every language is translated with the weights the others use.
		service.setPrecisions( {}, lg::translate::Precision::Full );
		test::expect( service.precisionFor( **japanese ) == lg::translate::Precision::Full );
		service.setPrecisions( { { "ja", lg::translate::Precision::Compact } }, lg::translate::Precision::Full );
		test::expect( service.precisionFor( **japanese ) == lg::translate::Precision::Compact );
		test::expect( service.precisionFor( **russian ) == lg::translate::Precision::Full );
	} );

} // namespace
