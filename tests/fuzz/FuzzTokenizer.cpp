#include <lexiglance/translate/Tokenizer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

	// OPUS-MT's vocabulary shape (tests/TestTranslate.cpp).
	constexpr std::string_view vocabulary = R"({
		"added_tokens": [{"id": 0, "content": "</s>", "special": true}, {"id": 1, "content": "<unk>", "special": true}],
		"model": {"type": "Unigram", "unk_id": 1, "vocab": [
			["</s>", 0.0], ["<unk>", 0.0], [",", -3.0], ["▁книг", -4.0], ["у", -2.0], ["▁к", -5.0], ["ниг", -6.0],
			["▁", -1.0], ["▁hello", -2.0], ["▁world", -2.0], ["▁.", -2.5], ["<pad>", 0.0], ["食べ", -3.0], ["る", -1.0]
		]}
	})";

} // namespace

// Before a NUL byte: a tokenizer.json (a downloaded model file); after it, or the whole input: text to translate.
extern "C" int LLVMFuzzerTestOneInput( const std::uint8_t* data, std::size_t size )
{
	namespace translate                          = lexiglance::translate;
	static const auto                   standard = translate::Tokenizer::parse( std::string( vocabulary ) );
	const std::string_view              input( reinterpret_cast<const char*>( data ), size );
	std::string_view                    text = input;
	std::optional<translate::Tokenizer> own;
	if ( const auto nul = input.find( '\0' ); nul != std::string_view::npos )
	{
		if ( auto parsed = translate::Tokenizer::parse( std::string( input.substr( 0, nul ) ) ) )
		{
			own = std::move( *parsed );
		}
		text = input.substr( nul + 1 );
	}
	const auto& tokenizer = own ? *own : *standard;
	const auto  ids       = tokenizer.encode( text );
	( void )tokenizer.decode( ids );
	// Whatever the model generates, in or out of the vocabulary.
	std::vector<std::int64_t> generated;
	for ( std::size_t at = 0; at + sizeof( std::int32_t ) <= text.size(); at += sizeof( std::int32_t ) )
	{
		std::int32_t id = 0;
		std::memcpy( &id, text.data() + at, sizeof( id ) );
		generated.push_back( id );
	}
	( void )tokenizer.decode( generated );
	return 0;
}
