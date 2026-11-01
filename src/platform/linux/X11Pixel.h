#ifndef LEXIGLANCE_PLATFORM_LINUX_X11PIXEL_H
#define LEXIGLANCE_PLATFORM_LINUX_X11PIXEL_H

#include <bit>
#include <cstdint>

namespace lexiglance::platform
{

	// One colour channel of an XImage's pixels, scaled to 8 bits whatever the visual's depth (16, 24, 30). Made once per
	// image: the mask's shift and width cost more than the pixel itself.
	class Channel
	{
	public:
		explicit Channel( unsigned long mask ) noexcept :
			mask_( mask ),
			shift_( mask == 0 ? 0U : static_cast<unsigned>( std::countr_zero( mask ) ) ),
			bits_( std::popcount( mask ) )
		{
		}

		[[nodiscard]] std::uint8_t operator()( unsigned long pixel ) const noexcept
		{
			// Non-TrueColor visuals have no channel masks.
			if ( bits_ == 0 )
			{
				return 0;
			}
			const unsigned long value = ( pixel & mask_ ) >> shift_;
			return static_cast<std::uint8_t>( bits_ >= 8 ? value >> static_cast<unsigned>( bits_ - 8 ) : value * 255 / ( ( 1UL << static_cast<unsigned>( bits_ ) ) - 1 ) );
		}

	private:
		unsigned long mask_;
		unsigned      shift_;
		int           bits_;
	};

	[[nodiscard]] inline std::uint8_t channel8( unsigned long pixel, unsigned long mask ) noexcept
	{
		return Channel( mask )( pixel );
	}

} // namespace lexiglance::platform

#endif // LEXIGLANCE_PLATFORM_LINUX_X11PIXEL_H
