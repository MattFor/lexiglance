#include <lexiglance/translate/Tokenizer.h>

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Utf8.h>

#include <algorithm>
#include <array>
#include <limits>

namespace lexiglance::translate
{

	namespace
	{

		constexpr std::string_view word_mark = "▁";

		// Unicode's White_Space characters, where the text is split into words.
		bool isWhitespace( char32_t c ) noexcept
		{
			return ( c >= 0x09 && c <= 0x0D ) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || ( c >= 0x2000 && c <= 0x200A ) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
			       c == 0x3000;
		}

		void replaceAll( std::string& text, std::string_view from, std::string_view to )
		{
			for ( std::size_t at = text.find( from ); at != std::string::npos; at = text.find( from, at + to.size() ) )
			{
				text.replace( at, from.size(), to );
			}
		}

	} // namespace

	Result<Tokenizer> Tokenizer::parse( std::string json )
	{
		auto document = json::Document::parse( std::move( json ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		const json::Value& root  = document->root();
		const json::Value& model = root["model"];
		if ( model["type"].asString() != "Unigram" )
		{
			return fail( "the tokenizer is not a unigram model, the only kind supported (OPUS-MT's)" );
		}

		Tokenizer tokenizer;
		for ( const json::Value& entry : model["vocab"].items() )
		{
			tokenizer.pieces_.emplace_back( entry[0].asString() );
			tokenizer.scores_.push_back( static_cast<float>( entry[1].asDouble() ) );
		}
		const std::size_t size = tokenizer.pieces_.size();
		if ( size == 0 )
		{
			return fail( "the tokenizer has no vocabulary" );
		}
		tokenizer.special_.assign( size, false );
		for ( const json::Value& added : root["added_tokens"].items() )
		{
			const auto id = added["id"].asInt( -1 );
			if ( id >= 0 && std::cmp_less( id, size ) && added["special"].asBool() )
			{
				tokenizer.special_[static_cast<std::size_t>( id )] = true;
			}
		}

		// Special tokens are never matched in the text ("</s>" written out is just letters).
		float lowest = 0.0F;
		for ( std::size_t id = 0; id < size; ++id )
		{
			if ( tokenizer.special_[id] || tokenizer.pieces_[id].empty() )
			{
				continue;
			}
			tokenizer.ids_.try_emplace( tokenizer.pieces_[id], static_cast<std::int64_t>( id ) );
			tokenizer.longest_ = std::max( tokenizer.longest_, tokenizer.pieces_[id].size() );
			lowest             = std::min( lowest, tokenizer.scores_[id] );
		}
		tokenizer.unknown_score_ = lowest - 10.0F;

		// Found by name: OPUS-MT's converted files give an unk_id that points at another piece.
		const auto named = [&]( std::string_view piece ) -> std::int64_t {
			const auto it = std::ranges::find( tokenizer.pieces_, piece );
			return it == tokenizer.pieces_.end() ? -1 : static_cast<std::int64_t>( it - tokenizer.pieces_.begin() );
		};
		tokenizer.unknown_ = named( "<unk>" );
		if ( tokenizer.unknown_ < 0 )
		{
			tokenizer.unknown_ = model["unk_id"].asInt( 0 );
		}
		tokenizer.eos_ = named( "</s>" );
		if ( tokenizer.eos_ < 0 )
		{
			return fail( "the tokenizer has no end of sequence (</s>)" );
		}
		return tokenizer;
	}

	std::vector<std::int64_t> Tokenizer::encode( std::string_view text ) const
	{
		std::vector<std::int64_t> ids;
		std::string               word;
		std::size_t               at = 0;
		while ( at < text.size() )
		{
			std::size_t next = at;
			if ( isWhitespace( utf8::decode( text, next ) ) )
			{
				at = next;
				continue;
			}
			std::size_t end = at;
			for ( std::size_t probe = at; probe < text.size() && !isWhitespace( utf8::decode( text, probe ) ); )
			{
				end = probe;
			}
			word.assign( word_mark ).append( text.substr( at, end - at ) );
			encodeWord( word, ids );
			at = end;
		}
		ids.push_back( eos_ );
		return ids;
	}

	void Tokenizer::encodeWord( std::string_view word, std::vector<std::int64_t>& out ) const
	{
		// Viterbi over the character boundaries: the best score of a cut up to each, and the piece it ends with.
		const std::size_t         n = word.size();
		std::vector<float>        best( n + 1, -std::numeric_limits<float>::infinity() );
		std::vector<std::int64_t> piece( n + 1, -1 );
		std::vector<std::size_t>  from( n + 1, 0 );
		best[0] = 0.0F;
		for ( std::size_t i = 0; i < n; )
		{
			std::size_t character = i;
			( void )utf8::decode( word, character );
			if ( best[i] == -std::numeric_limits<float>::infinity() )
			{
				i = character;
				continue;
			}
			bool covered = false;
			for ( std::size_t j = i; j < n && j - i < longest_; )
			{
				( void )utf8::decode( word, j );
				if ( const auto it = ids_.find( word.substr( i, j - i ) ); it != ids_.end() )
				{
					const float score = best[i] + scores_[static_cast<std::size_t>( it->second )];
					if ( score > best[j] )
					{
						best[j]  = score;
						piece[j] = it->second;
						from[j]  = i;
					}
					covered = covered || j == character;
				}
			}
			// A character no piece covers on its own is unknown.
			if ( !covered && best[i] + unknown_score_ > best[character] )
			{
				best[character]  = best[i] + unknown_score_;
				piece[character] = unknown_;
				from[character]  = i;
			}
			i = character;
		}

		std::vector<std::int64_t> reversed;
		for ( std::size_t at = n; at > 0; at = from[at] )
		{
			// Unknown characters in a row are one unknown piece.
			if ( piece[at] != unknown_ || reversed.empty() || reversed.back() != unknown_ )
			{
				reversed.push_back( piece[at] );
			}
		}
		out.insert( out.end(), reversed.rbegin(), reversed.rend() );
	}

	std::string Tokenizer::decode( std::span<const std::int64_t> ids ) const
	{
		std::string text;
		for ( const std::int64_t id : ids )
		{
			if ( id < 0 || std::cmp_greater_equal( id, pieces_.size() ) || special_[static_cast<std::size_t>( id )] || id == unknown_ || id == eos_ )
			{
				continue;
			}
			text.append( pieces_[static_cast<std::size_t>( id )] );
		}
		replaceAll( text, word_mark, " " );
		// English spacing, as Hugging Face's clean_up_tokenization_spaces leaves it.
		static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> cleanups{
			{ { " .", "." }, { " ?", "?" }, { " !", "!" }, { " ,", "," }, { " ' ", "'" }, { " n't", "n't" }, { " 'm", "'m" }, { " 's", "'s" }, { " 've", "'ve" }, { " 're", "'re" } }
		};
		for ( const auto& [from, to] : cleanups )
		{
			replaceAll( text, from, to );
		}
		const auto first = text.find_first_not_of( ' ' );
		const auto last  = text.find_last_not_of( ' ' );
		return first == std::string::npos ? std::string() : text.substr( first, last - first + 1 );
	}

} // namespace lexiglance::translate
