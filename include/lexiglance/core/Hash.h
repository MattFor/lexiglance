#ifndef LEXIGLANCE_CORE_HASH_H
#define LEXIGLANCE_CORE_HASH_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lexiglance
{

	namespace detail
	{

		constexpr std::uint64_t load64( std::string_view text, std::size_t offset ) noexcept
		{
			std::uint64_t v = 0;
			for ( std::size_t i = 0; i < 8; ++i )
			{
				v |= static_cast<std::uint64_t>( static_cast<unsigned char>( text[offset + i] ) ) << ( 8U * i );
			}
			return v;
		}

		constexpr std::uint64_t fmix64( std::uint64_t k ) noexcept
		{
			k ^= k >> 33U;
			k *= 0xff51afd7ed558ccdULL;
			k ^= k >> 33U;
			k *= 0xc4ceb9fe1a85ec53ULL;
			k ^= k >> 33U;
			return k;
		}

	} // namespace detail

	// Persisted in compiled dictionaries: changing it requires a dictionary format version bump.
	[[nodiscard]] constexpr std::uint64_t hash64( std::string_view text, std::uint64_t seed = 0x9E3779B97F4A7C15ULL ) noexcept
	{
		constexpr std::uint64_t c1 = 0x87c37b91114253d5ULL;
		constexpr std::uint64_t c2 = 0x4cf5ad432745937fULL;

		std::uint64_t h = seed ^ ( static_cast<std::uint64_t>( text.size() ) * 0xff51afd7ed558ccdULL );
		std::size_t   i = 0;

		for ( ; i + 8 <= text.size(); i += 8 )
		{
			std::uint64_t k = detail::load64( text, i );
			k *= c1;
			k = std::rotl( k, 31 );
			k *= c2;
			h ^= k;
			h = ( std::rotl( h, 27 ) * 5 ) + 0x52dce729;
		}

		std::uint64_t tail = 0;
		for ( std::size_t j = 0; i + j < text.size(); ++j )
		{
			tail |= static_cast<std::uint64_t>( static_cast<unsigned char>( text[i + j] ) ) << ( 8U * j );
		}
		tail *= c1;
		tail = std::rotl( tail, 31 );
		tail *= c2;
		h ^= tail;

		return detail::fmix64( h );
	}

	// Transparent hasher so unordered containers keyed by std::string accept std::string_view lookups.
	struct StringHash
	{
		using is_transparent = void;

		[[nodiscard]] std::size_t operator()( std::string_view text ) const noexcept
		{
			return static_cast<std::size_t>( hash64( text ) );
		}
	};

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_HASH_H
