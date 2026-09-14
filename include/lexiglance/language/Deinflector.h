#ifndef LEXIGLANCE_LANGUAGE_DEINFLECTOR_H
#define LEXIGLANCE_LANGUAGE_DEINFLECTOR_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lexiglance::lang
{

	struct Deinflection
	{
		static constexpr std::size_t max_chain = 8;

		std::string                          text;
		std::uint32_t                        conditions = 0;
		std::array<std::uint16_t, max_chain> chain{};
		std::uint8_t                         length = 0;
	};

	// Suffix rewriting engine. A rule turns an inflected suffix back into a dictionary suffix when the candidate satisfies
	// `conditions_in`; the result carries `conditions_out`. Candidates with conditions == 0 are unconstrained, which is
	// only the case for the source text itself.
	class Deinflector
	{
	public:
		struct Transform
		{
			std::string name;
			std::string description;
		};

		std::uint16_t addTransform( std::string name, std::string description );

		void addRule( std::string_view inflected, std::string_view deinflected, std::uint32_t conditions_in, std::uint32_t conditions_out, std::uint16_t transform );

		// Must be called once after all rules are added.
		void finalize();

		// Rules leave at least this many characters of the word before their dictionary ending (0: any number). Russian
		// words of one letter (в, к, я) never inflect, so there a lone letter is not taken for a stem.
		void setMinimumStem( std::size_t characters ) noexcept
		{
			minimum_stem_ = characters;
		}

		// Breadth-first expansion of all rule chains; `out[0]` is always the unmodified text.
		void deinflect( std::string_view text, std::vector<Deinflection>& out ) const;

		[[nodiscard]] std::span<const Transform> transforms() const noexcept
		{
			return transforms_;
		}

		[[nodiscard]] std::string_view transformName( std::uint16_t id ) const noexcept;

		[[nodiscard]] std::size_t ruleCount() const noexcept
		{
			return rules_.size();
		}

	private:
		struct Rule
		{
			std::string   inflected;
			std::string   deinflected;
			std::uint32_t conditions_in  = 0;
			std::uint32_t conditions_out = 0;
			std::uint16_t transform      = 0;
		};

		static constexpr char32_t kana_first = 0x3040;
		static constexpr char32_t kana_last  = 0x30FF;

		[[nodiscard]] std::span<const std::uint32_t> bucket( char32_t last ) const noexcept;

		std::size_t                                              minimum_stem_ = 0;
		std::vector<Transform>                                   transforms_;
		std::vector<Rule>                                        rules_;
		std::vector<std::vector<std::uint32_t>>                  kana_buckets_;
		std::unordered_map<char32_t, std::vector<std::uint32_t>> other_buckets_;
	};

} // namespace lexiglance::lang

#endif // LEXIGLANCE_LANGUAGE_DEINFLECTOR_H
