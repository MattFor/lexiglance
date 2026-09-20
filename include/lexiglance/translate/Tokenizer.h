#ifndef LEXIGLANCE_TRANSLATE_TOKENIZER_H
#define LEXIGLANCE_TRANSLATE_TOKENIZER_H

#include <lexiglance/core/Error.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lexiglance::translate
{

	// SentencePiece's unigram model as a Hugging Face tokenizer.json describes it (OPUS-MT's): the text is split at
	// whitespace, each word gets ▁ in front and is cut into the pieces whose scores add up highest. Ids are the model's.
	class Tokenizer
	{
	public:
		// From the text of a tokenizer.json.
		[[nodiscard]] static Result<Tokenizer> parse( std::string json );

		// The ids of the text's pieces, the end of the sequence after them.
		[[nodiscard]] std::vector<std::int64_t> encode( std::string_view text ) const;

		// The text of generated ids, without the special ones (end of sequence, padding, unknown).
		[[nodiscard]] std::string decode( std::span<const std::int64_t> ids ) const;

		[[nodiscard]] std::int64_t endOfSequence() const noexcept
		{
			return eos_;
		}

		[[nodiscard]] std::size_t size() const noexcept
		{
			return pieces_.size();
		}

	private:
		struct Hash
		{
			using is_transparent = void;

			[[nodiscard]] std::size_t operator()( std::string_view text ) const noexcept
			{
				return std::hash<std::string_view>{}( text );
			}
		};

		// The ids of one word (▁ and its letters), by the best-scoring cut.
		void encodeWord( std::string_view word, std::vector<std::int64_t>& out ) const;

		std::vector<std::string>                                             pieces_;
		std::vector<float>                                                   scores_;
		std::vector<bool>                                                    special_;
		std::unordered_map<std::string, std::int64_t, Hash, std::equal_to<>> ids_;
		// The longest piece in bytes: no cut needs to look further.
		std::size_t  longest_ = 0;
		std::int64_t unknown_ = 0;
		std::int64_t eos_     = 0;
		// What a character no piece covers costs, below every piece (as SentencePiece does it).
		float unknown_score_ = -100.0F;
	};

} // namespace lexiglance::translate

#endif // LEXIGLANCE_TRANSLATE_TOKENIZER_H
