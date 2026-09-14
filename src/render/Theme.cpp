#include <lexiglance/render/Theme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include <format>
#include <utility>
#include <vector>

namespace lexiglance::render
{

	namespace
	{

		constexpr int hexDigit( char c ) noexcept
		{
			if ( c >= '0' && c <= '9' )
			{
				return c - '0';
			}
			if ( c >= 'a' && c <= 'f' )
			{
				return c - 'a' + 10;
			}
			if ( c >= 'A' && c <= 'F' )
			{
				return c - 'A' + 10;
			}
			return -1;
		}

		constexpr Color rgb( unsigned value, double alpha = 1.0 ) noexcept
		{
			return {
				.r = static_cast<double>( ( value >> 16U ) & 0xFFU ) / 255.0,
				.g = static_cast<double>( ( value >> 8U ) & 0xFFU ) / 255.0,
				.b = static_cast<double>( value & 0xFFU ) / 255.0,
				.a = alpha,
			};
		}

	} // namespace

	std::optional<Color> Color::parse( std::string_view text ) noexcept
	{
		if ( !text.starts_with( '#' ) )
		{
			return std::nullopt;
		}
		text.remove_prefix( 1 );
		if ( !std::ranges::all_of( text, []( char c ) { return hexDigit( c ) >= 0; } ) )
		{
			return std::nullopt;
		}

		const auto channel = [&]( std::size_t index, std::size_t width ) {
			if ( width == 1 )
			{
				return static_cast<double>( hexDigit( text[index] ) * 17 ) / 255.0;
			}
			return static_cast<double>( ( hexDigit( text[index] ) * 16 ) + hexDigit( text[index + 1] ) ) / 255.0;
		};

		switch ( text.size() )
		{
			case 3:
				return Color{ .r = channel( 0, 1 ), .g = channel( 1, 1 ), .b = channel( 2, 1 ) };
			case 6:
				return Color{ .r = channel( 0, 2 ), .g = channel( 2, 2 ), .b = channel( 4, 2 ) };
			case 8:
				return Color{ .r = channel( 0, 2 ), .g = channel( 2, 2 ), .b = channel( 4, 2 ), .a = channel( 6, 2 ) };
			default:
				return std::nullopt;
		}
	}

	std::string Color::hex() const
	{
		const auto byte = []( double v ) { return static_cast<int>( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) ); };
		return std::format( "#{:02x}{:02x}{:02x}", byte( r ), byte( g ), byte( b ) );
	}

	Color mix( const Color& a, const Color& b, double t ) noexcept
	{
		return { .r = a.r + ( ( b.r - a.r ) * t ), .g = a.g + ( ( b.g - a.g ) * t ), .b = a.b + ( ( b.b - a.b ) * t ), .a = a.a + ( ( b.a - a.a ) * t ) };
	}

	namespace
	{

		double luminance( const Color& c ) noexcept
		{
			const auto linear = []( double v ) { return v <= 0.03928 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 ); };
			return ( 0.2126 * linear( c.r ) ) + ( 0.7152 * linear( c.g ) ) + ( 0.0722 * linear( c.b ) );
		}

		bool isDark( const Color& background ) noexcept
		{
			return luminance( background ) < 0.2;
		}

		// Colours that follow from the accent.
		void deriveAccent( Theme& theme )
		{
			const bool dark = isDark( theme.background );
			theme.on_accent = luminance( theme.accent ) > 0.4 ? rgb( 0x141414 ) : rgb( 0xffffff );
			theme.reading   = mix( theme.accent, theme.text, dark ? 0.25 : 0.2 );
			theme.chip      = mix( theme.accent, theme.background, dark ? 0.8 : 0.87 );
			theme.chip_text = mix( theme.accent, theme.text, dark ? 0.3 : 0.5 );
			theme.selection = { .r = theme.accent.r, .g = theme.accent.g, .b = theme.accent.b, .a = dark ? 0.42 : 0.3 };
		}

		// A whole palette from background, text and accent; tag colours stay Yomitan's.
		Theme derived( const Color& background, const Color& text, const Color& accent )
		{
			const bool dark       = isDark( background );
			Theme      theme      = dark ? Theme::dark() : Theme::light();
			theme.background      = background;
			theme.text            = text;
			theme.accent          = accent;
			theme.muted           = mix( text, background, 0.38 );
			theme.separator       = mix( text, background, 0.86 );
			theme.border          = mix( text, background, 0.7 );
			theme.scrollbar       = { .r = text.r, .g = text.g, .b = text.b, .a = 0.3 };
			theme.frequency_value = mix( text, background, dark ? 0.8 : 0.3 );
			theme.button          = mix( text, background, dark ? 0.88 : 0.92 );
			theme.button_icon     = mix( text, background, 0.25 );
			deriveAccent( theme );
			return theme;
		}

		Color opaque( Color color ) noexcept
		{
			color.a = 1.0;
			return color;
		}

	} // namespace

	double contrast( const Color& a, const Color& b ) noexcept
	{
		const double la = luminance( a );
		const double lb = luminance( b );
		return ( std::max( la, lb ) + 0.05 ) / ( std::min( la, lb ) + 0.05 );
	}

	Theme Theme::make( Scheme scheme, bool dark )
	{
		struct Base
		{
			unsigned background;
			unsigned text;
			unsigned accent;
		};
		const auto pick = [dark]( Base night, Base day ) { return dark ? night : day; };
		Base       base{};
		switch ( scheme )
		{
			case Scheme::Default:
				return dark ? Theme::dark() : Theme::light();
			case Scheme::Paper:
				base = pick( { .background = 0x2b2621, .text = 0xe8dcc8, .accent = 0xe39a64 }, { .background = 0xf7f0e3, .text = 0x3b3128, .accent = 0xa84b22 } );
				break;
			case Scheme::Nord:
				base = pick( { .background = 0x2e3440, .text = 0xe5e9f0, .accent = 0x88c0d0 }, { .background = 0xeceff4, .text = 0x2e3440, .accent = 0x4c6f9e } );
				break;
			case Scheme::Sakura:
				base = pick( { .background = 0x2b2027, .text = 0xf2dde5, .accent = 0xf28cb1 }, { .background = 0xfff6f8, .text = 0x3b2a31, .accent = 0xc23c6e } );
				break;
			case Scheme::Matcha:
				base = pick( { .background = 0x1f261c, .text = 0xdfe8d3, .accent = 0x9ccc6c }, { .background = 0xf4f7ee, .text = 0x28331f, .accent = 0x4b7a24 } );
				break;
			case Scheme::Midnight:
				base = pick( { .background = 0x000000, .text = 0xe4e4e4, .accent = 0x7aa2ff }, { .background = 0xf5f7ff, .text = 0x1a1d2b, .accent = 0x3355cc } );
				break;
			case Scheme::Contrast:
				base = pick( { .background = 0x000000, .text = 0xffffff, .accent = 0xffd400 }, { .background = 0xffffff, .text = 0x000000, .accent = 0x0033cc } );
				break;
		}
		Theme theme = derived( rgb( base.background ), rgb( base.text ), rgb( base.accent ) );
		if ( scheme == Scheme::Contrast )
		{
			theme.border    = theme.text;
			theme.muted     = mix( theme.text, theme.background, 0.2 );
			theme.separator = mix( theme.text, theme.background, 0.55 );
		}
		return theme;
	}

	Theme Theme::withColors( std::optional<Color> background_color, std::optional<Color> text_color, std::optional<Color> accent_color, std::optional<Color> border_color ) const
	{
		Theme theme = *this;
		if ( background_color || text_color )
		{
			theme = derived( opaque( background_color.value_or( background ) ), opaque( text_color.value_or( text ) ), opaque( accent_color.value_or( accent ) ) );
		}
		else if ( accent_color )
		{
			theme.accent = opaque( *accent_color );
			deriveAccent( theme );
		}
		if ( border_color )
		{
			theme.border = opaque( *border_color );
		}
		return theme;
	}

	Theme Theme::dark()
	{
		return {
			.background         = rgb( 0x1e1f22 ),
			.border             = rgb( 0x3c3f45 ),
			.text               = rgb( 0xdcdde0 ),
			.muted              = rgb( 0x9a9ca3 ),
			.separator          = rgb( 0x34363b ),
			.scrollbar          = rgb( 0xffffff, 0.28 ),
			.pill_text          = rgb( 0xffffff ),
			.tag_default        = rgb( 0x5d5f66 ),
			.tag_name           = rgb( 0xb6327a ),
			.tag_expression     = rgb( 0xc98a1f ),
			.tag_popular        = rgb( 0x1f6fc5 ),
			.tag_frequent       = rgb( 0x2f93b0 ),
			.tag_archaism       = rgb( 0xb8433f ),
			.tag_dictionary     = rgb( 0x8e55b3 ),
			.tag_frequency      = rgb( 0x3d8f47 ),
			.tag_part_of_speech = rgb( 0x4d4f55 ),
			.tag_pitch          = rgb( 0x6640be ),
			.frequency_value    = rgb( 0x2b2d31 ),
			.button             = rgb( 0x2c2e33 ),
			.button_icon        = rgb( 0xb9bcc3 ),
			.selection          = rgb( 0x4f8fe6, 0.42 ),
			.accent             = rgb( 0x6aa6f8 ),
			.on_accent          = rgb( 0x141414 ),
			.reading            = rgb( 0x8fb8f2 ),
			.chip               = rgb( 0x283548 ),
			.chip_text          = rgb( 0xa9c8f2 ),
		};
	}

	Theme Theme::light()
	{
		return {
			.background         = rgb( 0xffffff ),
			.border             = rgb( 0xc9cbd1 ),
			.text               = rgb( 0x26282c ),
			.muted              = rgb( 0x6c6f76 ),
			.separator          = rgb( 0xe3e5e8 ),
			.scrollbar          = rgb( 0x000000, 0.25 ),
			.pill_text          = rgb( 0xffffff ),
			.tag_default        = rgb( 0x8a8a91 ),
			.tag_name           = rgb( 0xb6327a ),
			.tag_expression     = rgb( 0xe0982d ),
			.tag_popular        = rgb( 0x0275d8 ),
			.tag_frequent       = rgb( 0x3fa9c9 ),
			.tag_archaism       = rgb( 0xd9534f ),
			.tag_dictionary     = rgb( 0xa35fcf ),
			.tag_frequency      = rgb( 0x4aa855 ),
			.tag_part_of_speech = rgb( 0x6b6e75 ),
			.tag_pitch          = rgb( 0x6640be ),
			.frequency_value    = rgb( 0x5d6168 ),
			.button             = rgb( 0xebedf0 ),
			.button_icon        = rgb( 0x50535a ),
			.selection          = rgb( 0x3d7fe0, 0.30 ),
			.accent             = rgb( 0x2a66c9 ),
			.on_accent          = rgb( 0xffffff ),
			.reading            = rgb( 0x2c5aa6 ),
			.chip               = rgb( 0xe4edfa ),
			.chip_text          = rgb( 0x24508f ),
		};
	}

	namespace
	{

		using Field = Color Theme::*;

		constexpr std::array<std::pair<std::string_view, Field>, 26> fields{ {
				{ "background", &Theme::background },
				{ "border", &Theme::border },
				{ "text", &Theme::text },
				{ "muted", &Theme::muted },
				{ "separator", &Theme::separator },
				{ "scrollbar", &Theme::scrollbar },
				{ "pill_text", &Theme::pill_text },
				{ "tag_default", &Theme::tag_default },
				{ "tag_name", &Theme::tag_name },
				{ "tag_expression", &Theme::tag_expression },
				{ "tag_popular", &Theme::tag_popular },
				{ "tag_frequent", &Theme::tag_frequent },
				{ "tag_archaism", &Theme::tag_archaism },
				{ "tag_dictionary", &Theme::tag_dictionary },
				{ "tag_frequency", &Theme::tag_frequency },
				{ "tag_part_of_speech", &Theme::tag_part_of_speech },
				{ "tag_pitch", &Theme::tag_pitch },
				{ "frequency_value", &Theme::frequency_value },
				{ "button", &Theme::button },
				{ "button_icon", &Theme::button_icon },
				{ "selection", &Theme::selection },
				{ "accent", &Theme::accent },
				{ "on_accent", &Theme::on_accent },
				{ "reading", &Theme::reading },
				{ "chip", &Theme::chip },
				{ "chip_text", &Theme::chip_text },
		} };

	} // namespace

	bool Theme::set( std::string_view name, const Color& color )
	{
		const auto* const it = std::ranges::find( fields, name, &std::pair<std::string_view, Field>::first );
		if ( it == fields.end() )
		{
			return false;
		}
		this->*( it->second ) = color;
		return true;
	}

	std::vector<std::string_view> Theme::colorNames()
	{
		std::vector<std::string_view> names;
		names.reserve( fields.size() );
		for ( const auto& [name, field] : fields )
		{
			names.push_back( name );
		}
		return names;
	}

	Color Theme::tag( std::string_view category ) const noexcept
	{
		if ( category == "name" )
		{
			return tag_name;
		}
		if ( category == "expression" )
		{
			return tag_expression;
		}
		if ( category == "popular" )
		{
			return tag_popular;
		}
		if ( category == "frequent" )
		{
			return tag_frequent;
		}
		if ( category == "archaism" )
		{
			return tag_archaism;
		}
		if ( category == "dictionary" )
		{
			return tag_dictionary;
		}
		if ( category == "frequency" )
		{
			return tag_frequency;
		}
		if ( category == "partOfSpeech" )
		{
			return tag_part_of_speech;
		}
		if ( category == "pronunciation-dictionary" )
		{
			return tag_pitch;
		}
		return tag_default;
	}

	namespace
	{

		Color fromHsv( double hue, double saturation, double value ) noexcept
		{
			const double c = value * saturation;
			const double h = std::fmod( hue, 360.0 ) / 60.0;
			const double x = c * ( 1.0 - std::abs( std::fmod( h, 2.0 ) - 1.0 ) );
			const double m = value - c;
			double       r = 0.0;
			double       g = 0.0;
			double       b = 0.0;
			switch ( static_cast<int>( h ) )
			{
				case 0:
					r = c;
					g = x;
					break;
				case 1:
					r = x;
					g = c;
					break;
				case 2:
					g = c;
					b = x;
					break;
				case 3:
					g = x;
					b = c;
					break;
				case 4:
					r = x;
					b = c;
					break;
				default:
					r = c;
					b = x;
					break;
			}
			return { .r = r + m, .g = g + m, .b = b + m, .a = 1.0 };
		}

	} // namespace

	Color autoHighlight( std::span<const std::uint32_t> pixels, double opacity, bool fill ) noexcept
	{
		const auto luma = []( std::uint32_t p ) { return ( ( ( ( p >> 16U ) & 0xFFU ) * 299 ) + ( ( ( p >> 8U ) & 0xFFU ) * 587 ) + ( ( p & 0xFFU ) * 114 ) ) / 1000; };

		// The background is the more common side of the brightness range; its average colour decides.
		std::array<int, 256> histogram{};
		for ( const std::uint32_t p : pixels )
		{
			++histogram[luma( p )];
		}
		const auto percentile = [&]( std::size_t percent ) {
			std::size_t seen = 0;
			for ( std::size_t v = 0; v < histogram.size(); ++v )
			{
				seen += static_cast<std::size_t>( histogram[v] );
				if ( seen * 100 >= pixels.size() * percent )
				{
					return static_cast<std::uint32_t>( v );
				}
			}
			return 255U;
		};
		const std::uint32_t split = ( percentile( 5 ) + percentile( 95 ) ) / 2;
		std::size_t         below = 0;
		for ( std::uint32_t v = 0; v < split; ++v )
		{
			below += static_cast<std::size_t>( histogram[v] );
		}
		const bool background_dark = below * 2 > pixels.size();

		double      r     = 0.0;
		double      g     = 0.0;
		double      b     = 0.0;
		std::size_t count = 0;
		for ( const std::uint32_t p : pixels )
		{
			if ( ( luma( p ) < split ) == background_dark )
			{
				r += ( p >> 16U ) & 0xFFU;
				g += ( p >> 8U ) & 0xFFU;
				b += p & 0xFFU;
				++count;
			}
		}
		if ( count == 0 )
		{
			r = g = b = 255.0;
			count     = 1;
		}
		r /= 255.0 * static_cast<double>( count );
		g /= 255.0 * static_cast<double>( count );
		b /= 255.0 * static_cast<double>( count );

		const double high       = std::max( { r, g, b } );
		const double low        = std::min( { r, g, b } );
		const double saturation = high > 0.0 ? ( high - low ) / high : 0.0;
		double       hue        = 0.0;
		if ( high > low )
		{
			if ( high == r )
			{
				hue = 60.0 * std::fmod( ( g - b ) / ( high - low ), 6.0 );
			}
			else if ( high == g )
			{
				hue = 60.0 * ( ( ( b - r ) / ( high - low ) ) + 2.0 );
			}
			else
			{
				hue = 60.0 * ( ( ( r - g ) / ( high - low ) ) + 4.0 );
			}
		}
		const bool dark = ( ( r * 0.299 ) + ( g * 0.587 ) + ( b * 0.114 ) ) < 0.5;
		double     pick = dark ? 42.0 : 215.0;
		if ( saturation > 0.25 )
		{
			pick = std::fmod( hue + 540.0, 360.0 );
		}
		Color color = fromHsv( pick, dark ? 0.75 : 0.85, dark || fill ? 1.0 : 0.85 );
		// An opaque fill would hide the text, so it falls back to a tint.
		color.a = 1.0;
		if ( fill )
		{
			color.a = opacity >= 0.95 ? 0.35 : opacity;
		}
		return color;
	}

	namespace
	{

		int channel( std::uint32_t pixel, unsigned shift ) noexcept
		{
			return static_cast<int>( ( pixel >> shift ) & 0xFFU );
		}

		// The largest difference in any channel.
		int difference( std::uint32_t pixel, const std::array<int, 3>& colour ) noexcept
		{
			return std::max( { std::abs( channel( pixel, 16U ) - colour[0] ), std::abs( channel( pixel, 8U ) - colour[1] ), std::abs( channel( pixel, 0U ) - colour[2] ) } );
		}

		// The commonest colour along the border of a block, and the share of border pixels close to it.
		std::pair<std::array<int, 3>, double> borderColour( std::span<const std::uint32_t> pixels, int width, int height, int tolerance )
		{
			const auto                 at = [&]( int x, int y ) { return pixels[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x )]; };
			std::vector<std::uint32_t> border;
			for ( int x = 0; x < width; ++x )
			{
				border.push_back( at( x, 0 ) );
				border.push_back( at( x, height - 1 ) );
			}
			for ( int y = 1; y + 1 < height; ++y )
			{
				border.push_back( at( 0, y ) );
				border.push_back( at( width - 1, y ) );
			}
			// Colours binned by the top four bits of each channel.
			const auto            bin = []( std::uint32_t p ) { return ( ( p >> 12U ) & 0xF00U ) | ( ( p >> 8U ) & 0xF0U ) | ( ( p >> 4U ) & 0xFU ); };
			std::array<int, 4096> bins{};
			for ( const std::uint32_t p : border )
			{
				++bins[bin( p )];
			}
			const auto         top = static_cast<std::uint32_t>( std::ranges::max_element( bins ) - bins.begin() );
			std::array<int, 3> sum{};
			int                count = 0;
			for ( const std::uint32_t p : border )
			{
				if ( bin( p ) == top )
				{
					sum[0] += channel( p, 16U );
					sum[1] += channel( p, 8U );
					sum[2] += channel( p, 0U );
					++count;
				}
			}
			if ( count == 0 )
			{
				return { {}, 0.0 };
			}
			const std::array<int, 3> colour{ sum[0] / count, sum[1] / count, sum[2] / count };
			const auto               close = std::ranges::count_if( border, [&]( std::uint32_t p ) { return difference( p, colour ) <= tolerance; } );
			return { colour, static_cast<double>( close ) / static_cast<double>( border.size() ) };
		}

	} // namespace

	Marker marker( std::span<const std::uint32_t> pixels, int width, int height )
	{
		constexpr int tolerance = 40;
		Marker        result;
		if ( width < 3 || height < 3 || pixels.size() != static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) )
		{
			return result;
		}
		const auto [background, share] = borderColour( pixels, width, height, tolerance );
		if ( share < 0.6 )
		{
			return result;
		}
		// Glyph pixels, and one pixel around them (their anti-aliased edges), stay uncovered.
		const auto  index = [&]( int x, int y ) { return ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x ); };
		std::size_t open  = 0;
		result.cover.assign( pixels.size(), 1 );
		for ( int y = 0; y < height; ++y )
		{
			for ( int x = 0; x < width; ++x )
			{
				if ( difference( pixels[index( x, y )], background ) <= tolerance )
				{
					continue;
				}
				for ( int ny = std::max( 0, y - 1 ); ny <= std::min( height - 1, y + 1 ); ++ny )
				{
					for ( int nx = std::max( 0, x - 1 ); nx <= std::min( width - 1, x + 1 ); ++nx )
					{
						open += result.cover[index( nx, ny )];
						result.cover[index( nx, ny )] = 0;
					}
				}
			}
		}
		// Mostly "glyph" means the background is not one colour after all.
		if ( open * 10 > pixels.size() * 7 )
		{
			result.cover.clear();
			return result;
		}
		result.usable     = true;
		result.background = { .r = background[0] / 255.0, .g = background[1] / 255.0, .b = background[2] / 255.0, .a = 1.0 };
		return result;
	}

} // namespace lexiglance::render
