#include <lexiglance/language/Deinflector.h>
#include <lexiglance/language/Language.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A language file from the user's languages folder, then everything a lookup asks of it.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	namespace lang = lexiglance::lang;
	auto language  = lang::parseLanguage( std::string_view( reinterpret_cast<const char*>( data ), size ) );
	if ( !language )
	{
		return 0;
	}
	const auto&                   defined = **language;
	std::vector<std::string_view> words( defined.sampleWords().begin(), defined.sampleWords().end() );
	words.insert( words.end(), { "", "книгами", "τους", "먹었어요", "食べた", "x" } );
	std::vector<lang::Deinflection> results;
	std::vector<lang::TextVariant>  variants;
	for ( const auto word : words )
	{
		results.clear();
		defined.deinflector().deinflect( word, results );
		variants.clear();
		defined.variants( U"ЁЛКИ Ὀδυσσεύς", variants );
		( void )defined.headword( word, word );
		( void )lang::wordStart( word, word.size() / 2, defined );
	}
	( void )defined.sampleText();
	( void )defined.ocrModels().paddleFile();
	return 0;
}
