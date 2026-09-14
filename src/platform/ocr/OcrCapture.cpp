#include "OcrCapture.h"

#include "Tesseract.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/ocr/Paddle.h>

#include <lexiglance/core/Hash.h>
#include <lexiglance/core/Log.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <chrono>
#include <iterator>
#include <limits>
#include <utility>

namespace lexiglance::platform
{

	namespace
	{

		std::string joinText( const std::vector<std::string>& items, std::string_view separator )
		{
			std::string out;
			for ( const std::string& item : items )
			{
				out.append( out.empty() ? "" : separator ).append( item );
			}
			return out;
		}

		std::string languageNames( const std::vector<const lang::Language*>& languages )
		{
			std::vector<std::string> names;
			names.reserve( languages.size() );
			for ( const lang::Language* language : languages )
			{
				names.emplace_back( language->name() );
			}
			return joinText( names, ", " );
		}

		// Whether a test text was read well enough: most of its characters, in order, whatever the spacing.
		bool similar( std::string_view read, std::string_view sample )
		{
			std::u32string a;
			std::u32string b;
			for ( const char32_t c : utf8::codepoints( read ) )
			{
				if ( c != U' ' )
				{
					a.push_back( c );
				}
			}
			for ( const char32_t c : utf8::codepoints( sample ) )
			{
				if ( c != U' ' )
				{
					b.push_back( c );
				}
			}
			// Longest common subsequence.
			std::vector<std::size_t> row( b.size() + 1, 0 );
			for ( const char32_t c : a )
			{
				std::size_t diagonal = 0;
				for ( std::size_t j = 1; j <= b.size(); ++j )
				{
					const std::size_t above = row[j];
					row[j]                  = c == b[j - 1] ? diagonal + 1 : std::max( row[j], row[j - 1] );
					diagonal                = above;
				}
			}
			return !b.empty() && row[b.size()] * 10 >= b.size() * 7;
		}

		// Tesseract reads glyphs best at about this height, surrounded by some blank margin.
		constexpr double target_height = 48.0;
		constexpr int    margin        = 12;

		struct Grab
		{
			Rect                      region;
			Point                     pointer;
			std::vector<std::uint8_t> gray;
			// Colour pixels (RGB), for PaddleOCR.
			std::vector<std::uint8_t> rgb;
			std::uint64_t             hash = 0;

			void setGray( std::size_t at, std::uint8_t value )
			{
				gray[at] = value;
				if ( !rgb.empty() )
				{
					rgb[at * 3]         = value;
					rgb[( at * 3 ) + 1] = value;
					rgb[( at * 3 ) + 2] = value;
				}
			}

			[[nodiscard]] std::size_t offset( int x, int y ) const
			{
				return ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( region.width ) ) + static_cast<std::size_t>( x );
			}

			[[nodiscard]] std::uint8_t at( int x, int y ) const
			{
				return gray[offset( x, y )];
			}
		};

		// Background and ink levels around the pointer, normalised to dark ink on a light background. Deciding them
		// locally keeps text readable even when the strip also covers differently coloured neighbours.
		struct Tone
		{
			bool dark       = false;
			int  background = 255;
			int  ink        = 0;
			// Two distinct background shades (a checkerboard, a pattern) rather than one.
			bool textured = false;

			[[nodiscard]] int level( std::uint8_t value ) const
			{
				return dark ? 255 - value : value;
			}
		};

		// The pointer's line within a grab and how much to enlarge it.
		struct Line
		{
			Rect crop;
			int  upscale = 2;
			// The line's own rows (columns for vertical text) within the crop, without padding or neighbours' strays.
			int core_from = 0;
			int core_to   = 0;
		};

		struct Recognition
		{
			Rect                   region;
			std::uint64_t          hash = 0;
			std::vector<OcrSymbol> symbols;
		};

		// The characters following the one under the pointer; kept alive as the capture handle for bounds().
		struct OcrLine
		{
			std::vector<Rect> boxes;
		};

		int floorTo( int value, int step )
		{
			return static_cast<int>( std::floor( static_cast<double>( value ) / step ) ) * step;
		}

		bool isSpace( std::string_view text )
		{
			return text.empty() || std::ranges::all_of( text, []( char c ) { return c == ' ' || c == '\t' || c == '\n'; } );
		}

		bool isSmallPunctuation( std::string_view text )
		{
			return text == "、" || text == "。" || text == "，" || text == "．" || text == "," || text == ".";
		}

		std::string dumpSymbols( const std::vector<OcrSymbol>& symbols )
		{
			std::string out;
			for ( const OcrSymbol& s : symbols )
			{
				std::format_to( std::back_inserter( out ), "{}@{},{},{}x{}#{}~{:.0f} ", s.text, s.box.x, s.box.y, s.box.width, s.box.height, s.line, s.confidence );
			}
			return out;
		}

		Rect unite( const Rect& a, const Rect& b )
		{
			const int left   = std::min( a.x, b.x );
			const int top    = std::min( a.y, b.y );
			const int right  = std::max( a.x + a.width, b.x + b.width );
			const int bottom = std::max( a.y + a.height, b.y + b.height );
			return { .x = left, .y = top, .width = right - left, .height = bottom - top };
		}

		Rect intersect( const Rect& a, const Rect& b )
		{
			const int left   = std::max( a.x, b.x );
			const int top    = std::max( a.y, b.y );
			const int right  = std::min( a.x + a.width, b.x + b.width );
			const int bottom = std::min( a.y + a.height, b.y + b.height );
			return { .x = left, .y = top, .width = std::max( 0, right - left ), .height = std::max( 0, bottom - top ) };
		}

		// Our own popup and highlight are on screen too. What a tinted highlight hides is known and put back; everything else
		// is painted over with the surrounding background.
		void mask( Grab& grab, const std::vector<Overlay>& overlays )
		{
			std::vector<Rect> local;
			for ( const Overlay& overlay : overlays )
			{
				if ( overlay.until != std::chrono::steady_clock::time_point{} )
				{
					continue;
				}
				const Rect hit = intersect( overlay.rect, grab.region );
				if ( hit.empty() )
				{
					continue;
				}
				if ( overlay.stipple > 0 )
				{
					// Every dot has undotted neighbours to its left and right.
					for ( int y = hit.y; y < hit.y + hit.height; ++y )
					{
						for ( int x = hit.x; x < hit.x + hit.width; ++x )
						{
							if ( !stippled( x, y, overlay.stipple ) )
							{
								continue;
							}
							const int         lx    = std::max( x - 1, grab.region.x ) - grab.region.x;
							const int         rx    = std::min( x + 1, grab.region.x + grab.region.width - 1 ) - grab.region.x;
							const int         ly    = y - grab.region.y;
							const std::size_t at    = grab.offset( x - grab.region.x, ly );
							const std::size_t left  = grab.offset( lx, ly );
							const std::size_t right = grab.offset( rx, ly );
							grab.gray[at]           = static_cast<std::uint8_t>( ( grab.gray[left] + grab.gray[right] ) / 2 );
							for ( std::size_t c = 0; c < 3 && !grab.rgb.empty(); ++c )
							{
								grab.rgb[( at * 3 ) + c] = static_cast<std::uint8_t>( ( grab.rgb[( left * 3 ) + c] + grab.rgb[( right * 3 ) + c] ) / 2 );
							}
						}
					}
					continue;
				}
				const auto& under = overlay.underneath;
				if ( under && under->size() == static_cast<std::size_t>( overlay.rect.width ) * static_cast<std::size_t>( overlay.rect.height ) )
				{
					for ( int y = hit.y; y < hit.y + hit.height; ++y )
					{
						for ( int x = hit.x; x < hit.x + hit.width; ++x )
						{
							grab.setGray( grab.offset( x - grab.region.x, y - grab.region.y ), ( *under )[( static_cast<std::size_t>( y - overlay.rect.y ) * static_cast<std::size_t>( overlay.rect.width ) ) + static_cast<std::size_t>( x - overlay.rect.x )] );
						}
					}
					continue;
				}
				local.push_back( { .x = hit.x - grab.region.x, .y = hit.y - grab.region.y, .width = hit.width, .height = hit.height } );
			}
			if ( local.empty() )
			{
				return;
			}

			std::array<int, 256> histogram{};
			for ( int y = 0; y < grab.region.height; ++y )
			{
				for ( int x = 0; x < grab.region.width; ++x )
				{
					if ( std::ranges::none_of( local, [&]( const Rect& r ) { return r.contains( { .x = x, .y = y } ); } ) )
					{
						++histogram[grab.at( x, y )];
					}
				}
			}
			const auto fill = static_cast<std::uint8_t>( std::ranges::max_element( histogram ) - histogram.begin() );
			for ( const Rect& r : local )
			{
				for ( int y = r.y; y < r.y + r.height; ++y )
				{
					for ( int x = r.x; x < r.x + r.width; ++x )
					{
						grab.setGray( grab.offset( x, y ), fill );
					}
				}
			}
		}

		Tone analyse( const Grab& grab, int unit, bool vertical )
		{
			const int reach_x = vertical ? unit : 2 * unit;
			const int reach_y = vertical ? 2 * unit : unit;
			const int x0      = std::clamp( grab.pointer.x - reach_x, 0, grab.region.width );
			const int x1      = std::clamp( grab.pointer.x + reach_x + 1, 0, grab.region.width );
			const int y0      = std::clamp( grab.pointer.y - reach_y, 0, grab.region.height );
			const int y1      = std::clamp( grab.pointer.y + reach_y + 1, 0, grab.region.height );

			std::array<int, 256> histogram{};
			int                  count = 0;
			for ( int y = y0; y < y1; ++y )
			{
				for ( int x = x0; x < x1; ++x )
				{
					++histogram[grab.at( x, y )];
					++count;
				}
			}
			if ( count == 0 )
			{
				return {};
			}
			const auto percentile = [&]( int percent ) {
				int seen = 0;
				for ( std::size_t v = 0; v < histogram.size(); ++v )
				{
					seen += histogram[v];
					if ( seen * 100 >= count * percent )
					{
						return static_cast<int>( v );
					}
				}
				return 255;
			};
			const int darkest   = percentile( 3 );
			const int brightest = percentile( 97 );
			const int split     = ( darkest + brightest ) / 2;

			// Which side is ink: the background forms long runs along the line (margins, the gaps between lines) while
			// ink only forms strokes. Counting pixels would fail on dense bold kanji, where ink can outnumber background.
			long       dark_long  = 0;
			long       light_long = 0;
			const auto flush      = [&]( int length, bool dark ) {
                if ( length >= unit )
                {
                    ( dark ? dark_long : light_long ) += length;
                }
			};
			const int outer_from = vertical ? x0 : y0;
			const int outer_to   = vertical ? x1 : y1;
			const int inner_from = vertical ? y0 : x0;
			const int inner_to   = vertical ? y1 : x1;
			for ( int o = outer_from; o < outer_to; ++o )
			{
				int  length = 0;
				bool dark   = false;
				for ( int i = inner_from; i < inner_to; ++i )
				{
					const bool here = std::cmp_less( vertical ? grab.at( o, i ) : grab.at( i, o ), split );
					if ( length > 0 && here != dark )
					{
						flush( length, dark );
						length = 0;
					}
					dark = here;
					++length;
				}
				flush( length, dark );
			}

			Tone tone;
			tone.dark = dark_long > light_long;

			// Background level: the most common brightness on the background side, in coarse buckets so that textures
			// such as checkerboards count as one.
			std::array<int, 8> buckets{};
			for ( int v = tone.dark ? 0 : split; v < ( tone.dark ? split : 256 ); ++v )
			{
				buckets[static_cast<std::size_t>( v / 32 )] += histogram[static_cast<std::size_t>( v )];
			}
			const auto bucket     = static_cast<int>( std::ranges::max_element( buckets ) - buckets.begin() );
			int        background = 0;
			int        weight     = 0;
			for ( int v = bucket * 32; v < ( bucket + 1 ) * 32; ++v )
			{
				background += v * histogram[static_cast<std::size_t>( v )];
				weight += histogram[static_cast<std::size_t>( v )];
			}
			background = weight > 0 ? background / weight : ( bucket * 32 ) + 16;

			tone.background = tone.level( static_cast<std::uint8_t>( background ) );
			tone.ink        = tone.level( static_cast<std::uint8_t>( tone.dark ? brightest : darkest ) );

			std::array<int, 32> shades{};
			for ( int v = tone.dark ? 0 : split; v < ( tone.dark ? split : 256 ); ++v )
			{
				shades[static_cast<std::size_t>( v / 8 )] += histogram[static_cast<std::size_t>( v )];
			}
			const auto main   = static_cast<int>( std::ranges::max_element( shades ) - shades.begin() );
			int        second = 0;
			for ( std::size_t i = 0; i < shades.size(); ++i )
			{
				if ( std::abs( static_cast<int>( i ) - main ) >= 2 )
				{
					second = std::max( second, shades[i] );
				}
			}
			tone.textured = second * 5 >= shades[static_cast<std::size_t>( main )];
			return tone;
		}

		// Which pixels of a grab are ink, in along/across coordinates (along being the reading direction).
		class InkMask
		{
		public:
			InkMask( const Grab& grab, const Tone& tone, bool vertical ) :
				along_( vertical ? grab.region.height : grab.region.width ),
				across_( vertical ? grab.region.width : grab.region.height ),
				bits_( static_cast<std::size_t>( along_ ) * static_cast<std::size_t>( across_ ) ),
				solid_( static_cast<std::size_t>( along_ ) )
			{
				const int threshold = ( tone.ink + tone.background ) / 2;
				for ( int a = 0; a < along_; ++a )
				{
					int count = 0;
					for ( int c = 0; c < across_; ++c )
					{
						const bool inked     = tone.level( vertical ? grab.at( c, a ) : grab.at( a, c ) ) < threshold;
						bits_[index( a, c )] = inked ? 1 : 0;
						count += inked ? 1 : 0;
					}
					// Window borders and dark panels, never text.
					solid_[static_cast<std::size_t>( a )] = count * 100 >= across_ * 97 ? 1 : 0;
				}
			}

			[[nodiscard]] int along() const noexcept
			{
				return along_;
			}

			[[nodiscard]] int across() const noexcept
			{
				return across_;
			}

			[[nodiscard]] bool ink( int a, int c ) const
			{
				return bits_[index( a, c )] != 0;
			}

			[[nodiscard]] bool solid( int a ) const
			{
				return solid_[static_cast<std::size_t>( a )] != 0;
			}

			// No ink in the rows [lo, hi] of column a.
			[[nodiscard]] bool blank( int a, int lo, int hi ) const
			{
				for ( int c = lo; c <= hi; ++c )
				{
					if ( ink( a, c ) )
					{
						return false;
					}
				}
				return true;
			}

		private:
			[[nodiscard]] std::size_t index( int a, int c ) const
			{
				return ( static_cast<std::size_t>( a ) * static_cast<std::size_t>( across_ ) ) + static_cast<std::size_t>( c );
			}

			int                       along_;
			int                       across_;
			std::vector<std::uint8_t> bits_;
			std::vector<std::uint8_t> solid_;
		};

		// The line's own rows and the crop's rows around them (padding of up to half a line).
		struct Rows
		{
			int lo   = 0;
			int hi   = 0;
			int from = 0;
			int to   = 0;
		};

		// The pointer's line, from the ink density across a wide stretch of the strip, so stray pixels between tightly set
		// lines do not join them.
		std::optional<Rows> lineRows( const InkMask& mask, int pa, int pc, int unit, std::pair<int, int> clear )
		{
			const auto [a0, a1] = clear;
			const int        s0 = std::max( a0, pa - ( 8 * unit ) );
			const int        s1 = std::min( a1, pa + ( 16 * unit ) );
			std::vector<int> density( static_cast<std::size_t>( mask.across() ) );
			for ( int c = 0; c < mask.across(); ++c )
			{
				for ( int a = s0; a < s1; ++a )
				{
					density[static_cast<std::size_t>( c )] += mask.ink( a, c ) ? 1 : 0;
				}
			}
			const int  width     = s1 - s0;
			const int  floor     = std::max( 1, width / 150 );
			const auto solid_row = [&]( int c ) { return density[static_cast<std::size_t>( c )] * 10 >= width * 9; };
			const auto text_row  = [&]( int c ) { return c >= 0 && c < mask.across() && !solid_row( c ) && density[static_cast<std::size_t>( c )] >= floor; };
			const auto near_ink  = [&]( int c ) {
                if ( !text_row( c ) )
                {
                    return false;
                }
                for ( int a = std::max( a0, pa - ( 2 * unit ) ); a < std::min( a1, pa + ( 2 * unit ) ); ++a )
                {
                    if ( mask.ink( a, c ) )
                    {
                        return true;
                    }
                }
                return false;
			};

			int start = -1;
			for ( int d = 0; d <= unit / 2 && start < 0; ++d )
			{
				if ( near_ink( pc - d ) )
				{
					start = pc - d;
				}
				else if ( near_ink( pc + d ) )
				{
					start = pc + d;
				}
			}
			if ( start < 0 )
			{
				return std::nullopt;
			}
			Rows rows{ .lo = start, .hi = start, .from = start, .to = start };
			while ( text_row( rows.lo - 1 ) )
			{
				--rows.lo;
			}
			while ( text_row( rows.hi + 1 ) )
			{
				++rows.hi;
			}
			if ( rows.hi - rows.lo + 1 < 5 )
			{
				return std::nullopt;
			}
			const int pad = std::max( 2, ( rows.hi - rows.lo + 1 ) / 2 );
			rows.from     = rows.lo;
			while ( rows.from > 0 && rows.lo - rows.from < pad && !text_row( rows.from - 1 ) && !solid_row( rows.from - 1 ) )
			{
				--rows.from;
			}
			rows.to = rows.hi;
			while ( rows.to < mask.across() - 1 && rows.to - rows.hi < pad && !text_row( rows.to + 1 ) && !solid_row( rows.to + 1 ) )
			{
				++rows.to;
			}
			return rows;
		}

		// Along the line: a little before the pointer and plenty after, on a grid for the cache. Glyphs cut by the strip's
		// edges would be read as garbage, so they are left out; elsewhere the crop never cuts through a glyph.
		std::pair<int, int> alongRange( const InkMask& mask, int origin, int pa, std::pair<int, int> clear, const Rows& rows )
		{
			const auto [a0, a1] = clear;
			const int  height   = rows.hi - rows.lo + 1;
			int        begin    = std::max( a0, floorTo( origin + pa - ( 8 * height ), 4 * height ) - origin );
			int        end      = std::min( a1, begin + ( 40 * height ) );
			const auto blank    = [&]( int a ) { return mask.blank( a, rows.lo, rows.hi ); };
			if ( begin == 0 && !blank( 0 ) )
			{
				while ( begin < pa && !blank( begin ) )
				{
					++begin;
				}
			}
			else
			{
				const int earliest = std::max( a0, begin - height );
				while ( begin > earliest && !blank( begin - 1 ) )
				{
					--begin;
				}
			}
			if ( end == mask.along() && !blank( end - 1 ) )
			{
				while ( end > pa + 1 && !blank( end - 1 ) )
				{
					--end;
				}
			}
			else
			{
				const int latest = std::min( a1, end + height );
				while ( end < latest && !blank( end ) )
				{
					++end;
				}
			}
			return { begin, end };
		}

		// Finds the pointer's line: its rows (columns for vertical text) and its extent along the strip up to solid areas
		// (window borders, dark panels).
		std::optional<Line> locate( const Grab& grab, const Tone& tone, int unit, bool vertical )
		{
			if ( tone.background - tone.ink < 24 )
			{
				return std::nullopt;
			}
			const InkMask mask( grab, tone, vertical );
			const int     pa = vertical ? grab.pointer.y : grab.pointer.x;
			const int     pc = vertical ? grab.pointer.x : grab.pointer.y;
			if ( mask.solid( pa ) )
			{
				return std::nullopt;
			}
			int a0 = pa;
			while ( a0 > 0 && !mask.solid( a0 - 1 ) )
			{
				--a0;
			}
			int a1 = pa + 1;
			while ( a1 < mask.along() && !mask.solid( a1 ) )
			{
				++a1;
			}
			const auto rows = lineRows( mask, pa, pc, unit, { a0, a1 } );
			if ( !rows )
			{
				return std::nullopt;
			}
			const auto [begin, end] = alongRange( mask, vertical ? grab.region.y : grab.region.x, pa, { a0, a1 }, *rows );
			const int height        = rows->hi - rows->lo + 1;

			Line line;
			line.upscale   = std::clamp( static_cast<int>( std::lround( target_height / height ) ), 1, 4 );
			line.crop      = vertical ? Rect{ .x = rows->from, .y = begin, .width = rows->to - rows->from + 1, .height = end - begin }
			                          : Rect{ .x = begin, .y = rows->from, .width = end - begin, .height = rows->to - rows->from + 1 };
			line.core_from = rows->lo - rows->from;
			line.core_to   = rows->hi - rows->from + 1;
			return line;
		}

		Grab crop( const Grab& grab, const Rect& local )
		{
			Grab out;
			out.region  = { .x = grab.region.x + local.x, .y = grab.region.y + local.y, .width = local.width, .height = local.height };
			out.pointer = { .x = grab.pointer.x - local.x, .y = grab.pointer.y - local.y };
			out.gray.reserve( static_cast<std::size_t>( local.width ) * static_cast<std::size_t>( local.height ) );
			for ( int y = local.y; y < local.y + local.height; ++y )
			{
				const auto row = grab.gray.begin() + static_cast<std::ptrdiff_t>( grab.offset( local.x, y ) );
				out.gray.insert( out.gray.end(), row, row + local.width );
			}
			const std::string_view bytes( reinterpret_cast<const char*>( out.gray.data() ), out.gray.size() );
			out.hash = hash64( bytes, static_cast<std::uint64_t>( out.region.x ) ^ ( static_cast<std::uint64_t>( out.region.y ) << 32U ) );
			return out;
		}

		// Grayscale, black ink on a white background, enlarged and padded: the input Tesseract handles best.
		std::vector<std::uint8_t> prepare( const Grab& grab, const Tone& tone, int upscale, int& width, int& height )
		{
			const int w = grab.region.width;
			const int h = grab.region.height;

			// Stretch ink to black and background to white; skip it when there is barely any contrast to stretch.
			const int  range   = tone.background - tone.ink;
			const bool stretch = range >= 40;
			// On textured backgrounds the top fifth of the range becomes pure background, so a checkerboard vanishes; plain
			// backgrounds keep every faint anti-aliased stroke (dakuten).
			const int  flat   = tone.textured ? std::max( 1, ( range * 4 ) / 5 ) : std::max( 1, range );
			const auto sample = [&]( int x, int y ) {
				const int v = tone.level( grab.at( std::clamp( x, 0, w - 1 ), std::clamp( y, 0, h - 1 ) ) );
				return stretch ? std::clamp( ( v - tone.ink ) * 255 / flat, 0, 255 ) : v;
			};

			width  = ( w * upscale ) + ( 2 * margin );
			height = ( h * upscale ) + ( 2 * margin );
			std::vector<std::uint8_t> out( static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ), 255 );

			// Bilinear enlargement.
			for ( int y = 0; y < h * upscale; ++y )
			{
				const double sy = ( ( y + 0.5 ) / upscale ) - 0.5;
				const int    y0 = static_cast<int>( std::floor( sy ) );
				const double fy = sy - y0;
				for ( int x = 0; x < w * upscale; ++x )
				{
					const double sx    = ( ( x + 0.5 ) / upscale ) - 0.5;
					const int    x0    = static_cast<int>( std::floor( sx ) );
					const double fx    = sx - x0;
					const double top   = ( sample( x0, y0 ) * ( 1.0 - fx ) ) + ( sample( x0 + 1, y0 ) * fx );
					const double below = ( sample( x0, y0 + 1 ) * ( 1.0 - fx ) ) + ( sample( x0 + 1, y0 + 1 ) * fx );
					out[( static_cast<std::size_t>( y + margin ) * static_cast<std::size_t>( width ) ) + static_cast<std::size_t>( x + margin )] =
							static_cast<std::uint8_t>( std::lround( ( top * ( 1.0 - fy ) ) + ( below * fy ) ) );
				}
			}
			return out;
		}

		struct Blob
		{
			int count = 0;
			int a0    = std::numeric_limits<int>::max();
			int a1    = std::numeric_limits<int>::min();
			int c0    = std::numeric_limits<int>::max();
			int c1    = std::numeric_limits<int>::min();
		};

		// The ink of a recognised line crop, in along/across coordinates, for placing its characters.
		class InkLine
		{
		public:
			InkLine( const Grab& grab, const Tone& tone, bool vertical, int core_from, int core_to ) :
				grab_( &grab ),
				tone_( tone ),
				vertical_( vertical ),
				along_( vertical ? grab.region.height : grab.region.width ),
				across_( vertical ? grab.region.width : grab.region.height ),
				columns_( static_cast<std::size_t>( along_ ), 0 ),
				core_from_( std::clamp( core_from, 0, across_ ) ),
				core_to_( std::clamp( core_to, core_from_, across_ ) )
			{
				for ( int a = 0; a < along_; ++a )
				{
					for ( int c = core_from_; c < core_to_; ++c )
					{
						if ( inked( a, c ) )
						{
							columns_[static_cast<std::size_t>( a )] = 1;
							low_                                    = std::min( low_, c );
							high_                                   = std::max( high_, c );
						}
					}
				}
			}

			[[nodiscard]] bool empty() const noexcept
			{
				return high_ < 0;
			}

			[[nodiscard]] int height() const noexcept
			{
				return high_ - low_ + 1;
			}

			[[nodiscard]] int length() const noexcept
			{
				return along_;
			}

			[[nodiscard]] int origin() const noexcept
			{
				return vertical_ ? grab_->region.y : grab_->region.x;
			}

			[[nodiscard]] bool inked( int a, int c ) const
			{
				return tone_.level( vertical_ ? grab_->at( c, a ) : grab_->at( a, c ) ) < ( tone_.ink + tone_.background ) / 2;
			}

			[[nodiscard]] bool blank( int a ) const
			{
				return a < 0 || a >= along_ || columns_[static_cast<std::size_t>( a )] == 0;
			}

			[[nodiscard]] Blob blob( int from, int to ) const
			{
				Blob blob;
				for ( int a = std::max( from, 0 ); a < std::min( to, along_ ); ++a )
				{
					for ( int c = low_; c <= high_; ++c )
					{
						if ( inked( a, c ) )
						{
							++blob.count;
							blob.a0 = std::min( blob.a0, a );
							blob.a1 = std::max( blob.a1, a );
							blob.c0 = std::min( blob.c0, c );
							blob.c1 = std::max( blob.c1, c );
						}
					}
				}
				return blob;
			}

			// 、 or 。 when a cell holds only a small blob in its leading, lower corner (upper right in vertical text).
			[[nodiscard]] std::string_view punctuation( const Blob& blob, int from, int to, int typical ) const
			{
				if ( blob.count == 0 || blob.count * 100 > typical * 45 )
				{
					return {};
				}
				const int along  = ( blob.a0 + blob.a1 ) / 2;
				const int across = ( blob.c0 + blob.c1 ) / 2;
				if ( ( along - from ) * 10 > ( to - from ) * 4 || ( across - low_ ) * 10 < height() * 6 )
				{
					return {};
				}
				// A ring has background at its centre, a tick has ink.
				return inked( along, across ) ? std::string_view( "、" ) : std::string_view( "。" );
			}

			// Whether [from, to) ends in 、 or 。, whose ink covers only the first half of their cell.
			[[nodiscard]] bool endsWithPunctuation( int from, int to, int typical ) const
			{
				int start = to - 1;
				while ( start > std::max( from, to - height() ) && !blank( start - 1 ) )
				{
					--start;
				}
				if ( start <= from || !blank( start - 1 ) )
				{
					return false;
				}
				const Blob last = blob( start, to );
				return last.count > 0 && last.count * 100 <= typical * 45 && ( ( ( last.c0 + last.c1 ) / 2 ) - low_ ) * 10 >= height() * 6;
			}

			// Stretches of ink separated by gaps wider than a character.
			[[nodiscard]] std::vector<std::pair<int, int>> runs() const
			{
				std::vector<std::pair<int, int>> out;
				for ( int a = 0; a < along_; ++a )
				{
					if ( columns_[static_cast<std::size_t>( a )] == 0 )
					{
						continue;
					}
					if ( !out.empty() && a - out.back().second <= height() )
					{
						out.back().second = a + 1;
					}
					else
					{
						out.emplace_back( a, a + 1 );
					}
				}
				return out;
			}

			[[nodiscard]] std::pair<int, int> span( const Rect& box ) const
			{
				const int from = ( vertical_ ? box.y : box.x ) - origin();
				return { from, from + ( vertical_ ? box.height : box.width ) };
			}

			[[nodiscard]] Rect box( int from, int to ) const
			{
				return vertical_ ? Rect{ .x = grab_->region.x + low_, .y = origin() + from, .width = height(), .height = to - from }
				                 : Rect{ .x = origin() + from, .y = grab_->region.y + low_, .width = to - from, .height = height() };
			}

		private:
			const Grab*               grab_;
			Tone                      tone_;
			bool                      vertical_;
			int                       along_;
			int                       across_;
			std::vector<std::uint8_t> columns_;
			int                       core_from_;
			int                       core_to_;
			int                       low_  = std::numeric_limits<int>::max();
			int                       high_ = -1;
		};

		// With the count right, a mark can still be missing next to a glyph the LSTM read twice ("領領", "んん"): where a cell
		// clearly holds 、 or 。 but got another character, the nearest doubled glyph at or after it is dropped and the mark
		// takes the cell.
		void repairStutter( std::vector<OcrSymbol>& cells, const InkLine& line, int typical )
		{
			for ( std::size_t i = 0; i < cells.size(); ++i )
			{
				if ( isSmallPunctuation( cells[i].text ) )
				{
					continue;
				}
				const auto [from, to] = line.span( cells[i].box );
				const auto mark       = line.punctuation( line.blob( from, to ), from, to, typical );
				if ( mark.empty() )
				{
					continue;
				}
				std::size_t doubled = 0;
				for ( std::size_t k = std::max<std::size_t>( i, 1 ); k < cells.size() && doubled == 0; ++k )
				{
					if ( cells[k].text == cells[k - 1].text && !isSmallPunctuation( cells[k].text ) )
					{
						doubled = k;
					}
				}
				if ( doubled == 0 )
				{
					continue;
				}
				std::vector<Rect> boxes;
				boxes.reserve( cells.size() );
				for ( const OcrSymbol& cell : cells )
				{
					boxes.push_back( cell.box );
				}
				cells.erase( cells.begin() + static_cast<std::ptrdiff_t>( doubled ) );
				cells.insert( cells.begin() + static_cast<std::ptrdiff_t>( i ), OcrSymbol{ .text = std::string( mark ), .box = {}, .line = 0, .confidence = 0.0F } );
				for ( std::size_t k = 0; k < cells.size(); ++k )
				{
					cells[k].box = boxes[k];
				}
			}
		}

		// Divides a run of ink into equal cells for its symbols, as Japanese is set on a grid. Tesseract tends to drop 、
		// and 。: when a grid with more cells fits the gaps between glyphs clearly better, the extra cells holding only a
		// small blob are those marks.
		std::vector<OcrSymbol> divide( const InkLine& line, int from, int to, const std::vector<const OcrSymbol*>& symbols )
		{
			const int  n        = static_cast<int>( symbols.size() );
			const int  typical  = std::max( 1, line.blob( from, to ).count / std::max( 1, n ) );
			const bool trailing = line.endsWithPunctuation( from, to, typical );
			const auto edge     = [&]( int cells, int i ) {
                const double pitch = static_cast<double>( to - from ) / ( cells - ( trailing && cells > 1 ? 0.5 : 0.0 ) );
                return from + static_cast<int>( std::lround( pitch * i ) );
			};
			const auto fit = [&]( int cells ) {
				int hits = 0;
				for ( int i = 1; i < cells; ++i )
				{
					const int b = edge( cells, i );
					hits += line.blank( b - 1 ) || line.blank( b ) || line.blank( b + 1 ) ? 1 : 0;
				}
				return cells < 2 ? 0.0 : static_cast<double>( hits ) / ( cells - 1 );
			};
			const auto assign = [&]( int cells ) -> std::optional<std::vector<OcrSymbol>> {
				std::vector<OcrSymbol> out;
				int                    missing = cells - n;
				int                    next    = 0;
				for ( int i = 0; i < cells; ++i )
				{
					const int  a   = edge( cells, i );
					const int  b   = edge( cells, i + 1 );
					const Rect box = line.box( a, b );
					if ( missing > 0 && cells - i > n - next )
					{
						const auto mark = line.punctuation( line.blob( a, b ), a, b, typical );
						if ( !mark.empty() && ( next >= n || !isSmallPunctuation( symbols[static_cast<std::size_t>( next )]->text ) ) )
						{
							out.push_back( { .text = std::string( mark ), .box = box, .line = 0, .confidence = 0.0F } );
							--missing;
							continue;
						}
					}
					if ( next < n )
					{
						OcrSymbol symbol = *symbols[static_cast<std::size_t>( next++ )];
						symbol.box       = box;
						symbol.line      = 0;
						out.push_back( std::move( symbol ) );
					}
				}
				if ( missing > 0 || next < n )
				{
					return std::nullopt;
				}
				return out;
			};

			int    cells = n;
			double best  = fit( n );
			for ( int extra = 1; extra <= 3; ++extra )
			{
				if ( const double score = fit( n + extra ); score > best + 0.1 )
				{
					best  = score;
					cells = n + extra;
				}
			}
			if ( cells != n )
			{
				if ( auto recovered = assign( cells ) )
				{
					return std::move( *recovered );
				}
			}
			auto plain = assign( n ).value_or( std::vector<OcrSymbol>{} );
			repairStutter( plain, line, typical );
			return plain;
		}

		using Cells = std::vector<std::pair<int, int>>;

		// Puts symbols into cells in reading order. Extra symbols (glyphs the LSTM read twice, garbage from a cut glyph) are
		// dropped; missing ones are 、 and 。, which Tesseract tends to drop, and go to cells holding only a small blob in
		// their leading, lower corner.
		std::vector<OcrSymbol> fill( const InkLine& line, const Cells& cells, std::vector<const OcrSymbol*> symbols, int typical, bool cut_front )
		{
			if ( cut_front && symbols.size() > cells.size() )
			{
				symbols.erase( symbols.begin() );
			}
			while ( symbols.size() > cells.size() )
			{
				const auto doubled = std::ranges::adjacent_find( symbols, []( const OcrSymbol* a, const OcrSymbol* b ) { return a->text == b->text && !isSmallPunctuation( a->text ); } );
				symbols.erase( doubled != symbols.end() ? std::next( doubled ) : std::prev( symbols.end() ) );
			}

			std::vector<OcrSymbol> out;
			std::size_t            missing = cells.size() - symbols.size();
			std::size_t            next    = 0;
			for ( std::size_t i = 0; i < cells.size(); ++i )
			{
				const auto [a, b] = cells[i];
				const Rect box    = line.box( a, b );
				if ( missing > 0 && cells.size() - i > symbols.size() - next )
				{
					const auto mark = line.punctuation( line.blob( a, b ), a, b, typical );
					if ( !mark.empty() && ( next >= symbols.size() || !isSmallPunctuation( symbols[next]->text ) ) )
					{
						out.push_back( { .text = std::string( mark ), .box = box, .line = 0, .confidence = 0.0F } );
						--missing;
						continue;
					}
				}
				if ( next < symbols.size() )
				{
					OcrSymbol symbol = *symbols[next++];
					symbol.box       = box;
					symbol.line      = 0;
					out.push_back( std::move( symbol ) );
				}
			}
			repairStutter( out, line, typical );
			return out;
		}

		struct Grid
		{
			double pitch = 0.0;
			double phase = 0.0;
			double score = 0.0;
		};

		// Japanese is set on a grid: its pitch and phase are those whose cell edges fall into the gaps between glyphs.
		Grid findGrid( const InkLine& line, const Cells& spans, std::size_t symbols )
		{
			const int h          = line.height();
			int       ink_length = 0;
			for ( const auto& [from, to] : spans )
			{
				ink_length += to - from;
			}
			Grid   best;
			double best_miss = std::numeric_limits<double>::max();
			if ( ink_length < h * 3 )
			{
				return best;
			}
			const auto gap = [&]( double at ) {
				const int a = static_cast<int>( std::lround( at ) );
				return line.blank( a - 1 ) || line.blank( a ) || line.blank( a + 1 );
			};
			// Pitches from 0.7 to 1.3 line heights in quarter pixels, phases in half pixels.
			for ( int quarter = ( h * 28 ) / 10; quarter <= ( h * 52 ) / 10; ++quarter )
			{
				const double pitch = quarter / 4.0;
				for ( int half = 0; half < quarter / 2; ++half )
				{
					const double phase = half / 2.0;
					int          hits  = 0;
					int          edges = 0;
					for ( const auto& [from, to] : spans )
					{
						for ( auto k = static_cast<long>( std::ceil( ( from - phase ) / pitch ) ); phase + ( static_cast<double>( k ) * pitch ) <= to; ++k )
						{
							++edges;
							hits += gap( phase + ( static_cast<double>( k ) * pitch ) ) ? 1 : 0;
						}
					}
					const double score = edges > 0 ? static_cast<double>( hits ) / edges : 0.0;
					// Among equally good grids, the one whose cell count is closest to Tesseract's symbol count.
					const double miss = std::abs( ( ink_length / pitch ) - static_cast<double>( symbols ) );
					if ( score > best.score + 0.02 || ( score > best.score - 0.02 && miss < best_miss ) )
					{
						best      = { .pitch = pitch, .phase = phase, .score = std::max( score, best.score ) };
						best_miss = miss;
					}
				}
			}
			return best;
		}

		// The grid's cells over one run; cells cut by the crop's edges are left out.
		std::vector<OcrSymbol> fillGrid( const InkLine& line, const Grid& grid, std::pair<int, int> run, const std::vector<const OcrSymbol*>& symbols )
		{
			Cells      cells;
			bool       cut_front = false;
			int        ink       = 0;
			const auto first     = static_cast<int>( std::floor( ( run.first - grid.phase ) / grid.pitch ) );
			const auto last      = static_cast<int>( std::ceil( ( run.second - grid.phase ) / grid.pitch ) );
			for ( int j = first; j < last; ++j )
			{
				const int  a    = static_cast<int>( std::lround( grid.phase + ( j * grid.pitch ) ) );
				const int  b    = static_cast<int>( std::lround( grid.phase + ( ( j + 1 ) * grid.pitch ) ) );
				const Blob blob = line.blob( a, b );
				if ( blob.count < 3 )
				{
					continue;
				}
				if ( a < 0 )
				{
					cut_front = true;
					continue;
				}
				if ( b > line.length() )
				{
					continue;
				}
				cells.emplace_back( a, b );
				ink += blob.count;
			}
			if ( cells.empty() )
			{
				return {};
			}
			auto out = fill( line, cells, symbols, std::max( 1, ink / static_cast<int>( cells.size() ) ), cut_front );
			if ( log::enabled( log::Level::Debug ) )
			{
				std::string text;
				for ( const auto& symbol : out )
				{
					text.append( symbol.text );
				}
				log::debug( "ocr: run {}-{}: {} symbols, {} cells{}, pitch {:.2f} phase {:.1f} -> «{}»", run.first, run.second, symbols.size(), cells.size(), cut_front ? " (cut front)" : "", grid.pitch, grid.phase, text );
			}
			return out;
		}

		// Tesseract's symbol boxes are often offset or merged, so characters are placed on the ink instead: each symbol goes
		// to the run of ink its box is nearest to (keeping reading order; vertical text has no usable boxes, so there they
		// are shared out by run length), and each run is divided into cells.
		std::vector<OcrSymbol> place( const std::vector<OcrSymbol>& symbols, const Grab& grab, const Tone& tone, bool vertical, bool boxes, std::pair<int, int> core )
		{
			const InkLine                 line( grab, tone, vertical, core.first, core.second );
			std::vector<const OcrSymbol*> visible;
			for ( const OcrSymbol& symbol : symbols )
			{
				if ( !isSpace( symbol.text ) )
				{
					visible.push_back( &symbol );
				}
			}
			if ( line.empty() || visible.empty() )
			{
				return {};
			}

			const auto                                 spans = line.runs();
			std::vector<std::vector<const OcrSymbol*>> members( spans.size() );
			int                                        total = 0;
			for ( const auto& [from, to] : spans )
			{
				total += to - from;
			}
			std::size_t current = 0;
			for ( std::size_t i = 0; i < visible.size(); ++i )
			{
				std::size_t chosen = 0;
				if ( boxes )
				{
					const auto& box    = visible[i]->box;
					const int   centre = ( vertical ? box.y + ( box.height / 2 ) : box.x + ( box.width / 2 ) ) - line.origin();
					int         best   = std::numeric_limits<int>::max();
					for ( std::size_t r = 0; r < spans.size(); ++r )
					{
						const int distance = std::max( { 0, spans[r].first - centre, centre - spans[r].second } );
						if ( distance < best )
						{
							best   = distance;
							chosen = r;
						}
					}
				}
				else
				{
					const double target = ( static_cast<double>( i ) + 0.5 ) * total / static_cast<double>( visible.size() );
					double       seen   = 0.0;
					while ( chosen + 1 < spans.size() && seen + ( spans[chosen].second - spans[chosen].first ) < target )
					{
						seen += spans[chosen].second - spans[chosen].first;
						++chosen;
					}
				}
				current = std::max( current, chosen );
				members[current].push_back( visible[i] );
			}

			const Grid grid = findGrid( line, spans, visible.size() );
			log::debug( "ocr: grid pitch {:.2f}, phase {:.1f}, fit {:.2f}", grid.pitch, grid.phase, grid.score );
			std::vector<OcrSymbol> placed;
			for ( std::size_t r = 0; r < spans.size(); ++r )
			{
				if ( !members[r].empty() )
				{
					auto part = grid.score >= 0.8 ? fillGrid( line, grid, spans[r], members[r] ) : divide( line, spans[r].first, spans[r].second, members[r] );
					placed.insert( placed.end(), std::make_move_iterator( part.begin() ), std::make_move_iterator( part.end() ) );
				}
			}
			return placed;
		}

		// Our own windows in a grab (in its coordinates): all of them, and those that show nothing of what is below.
		struct Covered
		{
			std::vector<Rect> all;
			std::vector<Rect> blank;
		};

		// Text boxes found a moment ago are reused while the pixels change (video, games), and only lines are read again.
		constexpr auto box_reuse = std::chrono::milliseconds( 300 );

		// A region read with PaddleOCR: its text boxes, and each box's line once it was needed.
		struct PaddleRegion
		{
			Rect                  region;
			std::uint64_t         hash = 0;
			std::vector<ocr::Box> boxes;
			// Boxes from this index on come from the looser second pass.
			std::size_t                               strict = 0;
			std::vector<std::optional<ocr::TextLine>> lines;
			std::chrono::steady_clock::time_point     detected;
			// The pixels when the boxes were found, where our own windows were then (region coordinates), and which of
			// those showed nothing of what is below (the popup; a highlight's pixels are put back).
			std::vector<std::uint8_t> pixels;
			std::vector<Rect>         hidden;
			std::vector<Rect>         blank;
		};

		class OcrCapture final : public TextCapture
		{
		public:
			OcrCapture( std::unique_ptr<ScreenReader> screen, std::unique_ptr<TesseractEngine> engine, std::unique_ptr<ocr::PaddleOcr> paddle, const OcrOptions& options, const std::string& models, std::vector<const lang::Language*> languages ) :
				screen_( std::move( screen ) ),
				engine_( std::move( engine ) ),
				paddle_( std::move( paddle ) ),
				overlays_( options.overlays ),
				vertical_( options.vertical ),
				unit_( std::max( 16, static_cast<int>( std::lround( 24.0 * options.scale ) ) ) ),
				languages_( std::move( languages ) ),
				description_( paddle_ ? std::format( "OCR (PaddleOCR: {})", languageNames( languages_ ) ) : std::format( "OCR ({}, {}, tesseract {})", models, options.model, engine_ ? engine_->version() : "?" ) )
			{
			}

			~OcrCapture() override = default;

			OcrCapture( const OcrCapture& )            = delete;
			OcrCapture& operator=( const OcrCapture& ) = delete;
			OcrCapture( OcrCapture&& )                 = delete;
			OcrCapture& operator=( OcrCapture&& )      = delete;

			[[nodiscard]] std::string_view name() const noexcept override
			{
				return "ocr";
			}

			[[nodiscard]] std::string describe() const override
			{
				return description_;
			}

			std::optional<CapturedText> capture( Point point, const WindowInfo& window, std::size_t max_chars ) override
			{
				if ( paddle_ )
				{
					return capturePaddle( point, window, max_chars );
				}
				const auto grab = grabAround( point, window );
				if ( grab.gray.empty() )
				{
					log::debug( "ocr: nothing to read at {},{} ({}x{})", point.x, point.y, grab.region.width, grab.region.height );
					return std::nullopt;
				}
				const Tone tone = analyse( grab, unit_, vertical_ );
				const auto line = locate( grab, tone, unit_, vertical_ );
				if ( !line )
				{
					log::debug( "ocr: no text line at {},{} (background {}, ink {})", point.x, point.y, tone.background, tone.ink );
					return std::nullopt;
				}
				return select( recognition( crop( grab, line->crop ), tone, line->upscale, { line->core_from, line->core_to } ), point, max_chars );
			}

			std::optional<Rect> bounds( const CapturedText& text, std::size_t length ) override
			{
				const auto* line = static_cast<const OcrLine*>( text.handle.get() );
				if ( line == nullptr || line->boxes.empty() || length == 0 )
				{
					return std::nullopt;
				}
				Rect area = line->boxes.front();
				for ( std::size_t i = 1; i < std::min( length, line->boxes.size() ); ++i )
				{
					area = unite( area, line->boxes[i] );
				}
				return area;
			}

			// Reads words of each language drawn for the purpose, with the same recognisers the captures use.
			void diagnose( std::vector<health::Check>& out ) override
			{
				health::Check            check{ .id = "ocr", .title = "Text in images, games and videos (OCR)" };
				std::vector<std::string> good;
				std::vector<std::string> bad;
				const auto               started = std::chrono::steady_clock::now();
				for ( const lang::Language* language : languages_ )
				{
					std::string sample;
					for ( const std::string_view word : language->sampleWords() )
					{
						sample.append( sample.empty() ? "" : " " ).append( word );
					}
					const auto raster = render::rasterizeText( sample, 40.0 );
					if ( raster.pixels.empty() )
					{
						continue;
					}
					const std::string read = readTest( raster.pixels, raster.width, raster.height );
					( similar( read, sample ) ? good : bad ).push_back( std::format( "{} «{}»", language->name(), read ) );
				}
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();

				if ( good.empty() && bad.empty() )
				{
					check.status = health::Severity::Warning;
					check.detail = std::format( "{} is loaded, but no test text could be drawn to check it.", description_ );
				}
				else if ( !bad.empty() )
				{
					check.status = health::Severity::Error;
					check.detail = std::format( "{} misread its test text ({}): the model may be damaged. Download it again.", description_, joinText( bad, "; " ) );
					check.fix    = "open-scanning";
				}
				else
				{
					check.detail = std::format( "{} read its test text in {} ms: {}.", description_, elapsed, joinText( good, "; " ) );
				}
				out.push_back( std::move( check ) );
			}

		private:
			// What the recogniser reads in a drawing (0xRRGGBB pixels).
			[[nodiscard]] std::string readTest( const std::vector<std::uint32_t>& pixels, int width, int height )
			{
				std::string read;
				if ( paddle_ )
				{
					ocr::Image image{ .width = width, .height = height, .rgb = {} };
					image.rgb.reserve( pixels.size() * 3 );
					for ( const std::uint32_t pixel : pixels )
					{
						image.rgb.push_back( static_cast<std::uint8_t>( ( pixel >> 16U ) & 0xFFU ) );
						image.rgb.push_back( static_cast<std::uint8_t>( ( pixel >> 8U ) & 0xFFU ) );
						image.rgb.push_back( static_cast<std::uint8_t>( pixel & 0xFFU ) );
					}
					for ( const auto& box : paddle_->detect( image ) )
					{
						for ( const auto& character : paddle_->recognize( image, box ).characters )
						{
							read += character.text;
						}
					}
				}
				else if ( engine_ )
				{
					std::vector<std::uint8_t> gray;
					gray.reserve( pixels.size() );
					for ( const std::uint32_t pixel : pixels )
					{
						gray.push_back( static_cast<std::uint8_t>( pixel & 0xFFU ) );
					}
					for ( const auto& symbol : engine_->recognize( gray, width, height ) )
					{
						read += symbol.text;
					}
				}
				return read;
			}

			// PaddleOCR: text boxes in a region around the pointer, then the box under it read as a line. Regions sit on a
			// grid, so moving along a line keeps hitting the cache.
			std::optional<CapturedText> capturePaddle( Point point, const WindowInfo& window, std::size_t max_chars )
			{
				const int u      = unit_;
				Rect      region = { .x = floorTo( point.x - ( 10 * u ), 8 * u ), .y = floorTo( point.y - ( 10 * u ), 4 * u ), .width = 44 * u, .height = 20 * u };
				region           = intersect( region, screen_->bounds() );
				if ( const auto bounds = windowRect( window.id ) )
				{
					region = intersect( region, *bounds );
				}
				if ( region.width < 16 || region.height < 16 || !region.contains( point ) )
				{
					return std::nullopt;
				}
				Grab grab = grabRegion( region, point );
				if ( grab.rgb.empty() )
				{
					return std::nullopt;
				}
				const Covered       covered = coveredIn( grab );
				const std::uint64_t hash    = contentHash( grab, covered.all );
				const Point         local{ .x = point.x - region.x, .y = point.y - region.y };

				PaddleRegion* cached = nullptr;
				PaddleRegion* recent = nullptr;
				const auto    now    = std::chrono::steady_clock::now();
				for ( PaddleRegion& entry : paddle_cache_ )
				{
					if ( entry.region.x != region.x || entry.region.y != region.y || entry.region.width != region.width || entry.region.height != region.height )
					{
						continue;
					}
					// Unchanged but for our own windows moving, unless the pointer is on something they hid back then.
					const bool   revealed = std::ranges::any_of( entry.blank, [&]( const Rect& r ) { return r.contains( local ); } );
					const Change changed  = entry.hash == hash || revealed ? Change{} : differences( entry, grab.rgb, covered.all );
					const bool   same     = entry.hash == hash || ( !revealed && changed.runs == 0 );
					if ( !same )
					{
						log::debug( "ocr: region {},{} {}x{} read again ({})", region.x, region.y, region.width, region.height, revealed ? std::string( "pointer on text the popup hid" ) : std::format( "{} rows changed, {} to {}", changed.runs, region.y + changed.first, region.y + changed.last ) );
					}
					if ( same )
					{
						entry.hash = hash;
						cached     = &entry;
						break;
					}
					if ( now - entry.detected < box_reuse )
					{
						recent = &entry;
					}
				}
				if ( cached == nullptr && recent != nullptr )
				{
					recent->hash = hash;
					recent->lines.assign( recent->boxes.size(), std::nullopt );
					cached = recent;
				}
				const ocr::Image image{ .width = region.width, .height = region.height, .rgb = std::move( grab.rgb ) };
				if ( cached == nullptr )
				{
					if ( paddle_cache_.size() >= 4 )
					{
						paddle_cache_.erase( paddle_cache_.begin() );
					}
					PaddleRegion entry{ .region = region, .hash = hash, .boxes = paddle_->detect( image ), .strict = 0, .lines = {}, .detected = now, .pixels = image.rgb, .hidden = covered.all, .blank = covered.blank };
					entry.strict = entry.boxes.size();
					// Sparse glyphs (dotted and thin pixel fonts) only stand out with lower thresholds.
					std::ranges::move( paddle_->redetect( 0.15F, 0.3F ), std::back_inserter( entry.boxes ) );
					entry.lines.resize( entry.boxes.size() );
					log::debug( "ocr: {}+{} text boxes in {}x{} at {},{}", entry.strict, entry.boxes.size() - entry.strict, region.width, region.height, region.x, region.y );
					paddle_cache_.push_back( std::move( entry ) );
					cached = &paddle_cache_.back();
				}

				// The box under the pointer, or the nearest one within half a unit; a box of the looser pass only when nothing
				// else fits, and then only right under the pointer.
				const auto nearest = [&]( std::size_t from, std::size_t to, int reach ) {
					std::optional<std::size_t> chosen;
					for ( std::size_t i = from; i < to; ++i )
					{
						const auto& b        = cached->boxes[i];
						const int   distance = std::max( { 0, b.inner_x - local.x, local.x - ( b.inner_x + b.inner_width ), b.inner_y - local.y, local.y - ( b.inner_y + b.inner_height ) } );
						if ( distance < reach )
						{
							reach  = distance;
							chosen = i;
						}
					}
					return chosen;
				};
				auto       chosen = nearest( 0, cached->strict, ( u / 2 ) + 1 );
				const bool loose  = !chosen;
				if ( loose )
				{
					chosen = nearest( cached->strict, cached->boxes.size(), 1 );
				}
				if ( !chosen )
				{
					return std::nullopt;
				}
				auto& line = cached->lines[*chosen];
				if ( !line )
				{
					line      = paddle_->recognize( image, cached->boxes[*chosen], vertical_ );
					line->box = inked( image, line->box, line->vertical );
					if ( log::enabled( log::Level::Debug ) )
					{
						std::string text;
						for ( const auto& c : line->characters )
						{
							text.append( c.text );
						}
						log::debug( "ocr: {}box {}x{} ({}) «{}»", loose ? "loose " : "", line->box.width, line->box.height, line->vertical ? "vertical" : "horizontal", text );
					}
				}
				return selectLine( *line, region, local, max_chars, loose ? 0.8F : 0.6F );
			}

			[[nodiscard]] std::vector<Overlay> liveOverlays() const
			{
				if ( !overlays_ )
				{
					return {};
				}
				auto       overlays = overlays_();
				const auto now      = std::chrono::steady_clock::now();
				std::erase_if( overlays, [&]( const Overlay& overlay ) { return overlay.until != std::chrono::steady_clock::time_point{} && overlay.until < now; } );
				return overlays;
			}

			[[nodiscard]] Covered coveredIn( const Grab& grab ) const
			{
				Covered covered;
				for ( const Overlay& overlay : liveOverlays() )
				{
					const Rect hit = intersect( overlay.rect, grab.region );
					if ( hit.empty() )
					{
						continue;
					}
					const Rect local{ .x = hit.x - grab.region.x, .y = hit.y - grab.region.y, .width = hit.width, .height = hit.height };
					covered.all.push_back( local );
					if ( !overlay.underneath && overlay.stipple == 0 && overlay.until == std::chrono::steady_clock::time_point{} )
					{
						covered.blank.push_back( local );
					}
				}
				return covered;
			}

			// Calls visit( y, from, to ) for every run of pixels of a width x height block outside `covered`.
			template <typename Visit>
			static void forVisible( int width, int height, std::vector<Rect> covered, const Visit& visit )
			{
				std::ranges::sort( covered, {}, &Rect::x );
				for ( int y = 0; y < height; ++y )
				{
					int x = 0;
					for ( const Rect& r : covered )
					{
						if ( y >= r.y && y < r.y + r.height && r.x + r.width > x )
						{
							if ( r.x > x )
							{
								visit( y, x, r.x );
							}
							x = std::max( x, r.x + r.width );
						}
					}
					if ( width > x )
					{
						visit( y, x, width );
					}
				}
			}

			// The region's pixels, leaving out our own windows: they move with every lookup, the text under them does not.
			[[nodiscard]] static std::uint64_t contentHash( const Grab& grab, const std::vector<Rect>& hidden )
			{
				std::uint64_t hash = static_cast<std::uint64_t>( grab.region.x ) ^ ( static_cast<std::uint64_t>( grab.region.y ) << 32U );
				forVisible( grab.region.width, grab.region.height, hidden, [&]( int y, int from, int to ) {
					const auto* data = reinterpret_cast<const char*>( grab.rgb.data() ) + ( grab.offset( from, y ) * 3 );
					hash             = hash64( std::string_view( data, static_cast<std::size_t>( to - from ) * 3 ), hash );
				} );
				return hash;
			}

			// Runs of pixels outside our own windows (now and when the region was read) that differ from back then, and the
			// first and last row they are on.
			struct Change
			{
				std::size_t runs  = 0;
				int         first = 0;
				int         last  = 0;
			};

			[[nodiscard]] static Change differences( const PaddleRegion& entry, const std::vector<std::uint8_t>& rgb, const std::vector<Rect>& hidden )
			{
				Change change;
				if ( entry.pixels.size() != rgb.size() )
				{
					change.runs = rgb.size();
					return change;
				}
				std::vector<Rect> covered = hidden;
				covered.insert( covered.end(), entry.hidden.begin(), entry.hidden.end() );
				forVisible( entry.region.width, entry.region.height, std::move( covered ), [&]( int y, int from, int to ) {
					const std::size_t at = ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( entry.region.width ) ) + static_cast<std::size_t>( from ) ) * 3;
					if ( std::memcmp( entry.pixels.data() + at, rgb.data() + at, static_cast<std::size_t>( to - from ) * 3 ) != 0 )
					{
						change.first = change.runs == 0 ? y : change.first;
						change.last  = y;
						++change.runs;
					}
				} );
				return change;
			}

			// The detector finds a shrunk core of the text, and recognition reads a generous margin around it. What is
			// highlighted should be the glyphs themselves, so the core grows over the rows (columns for vertical text) that
			// hold ink, within that margin.
			[[nodiscard]] static ocr::Box inked( const ocr::Image& image, ocr::Box box, bool vertical )
			{
				const int  across0 = vertical ? box.x : box.y;
				const int  across1 = vertical ? box.x + box.width : box.y + box.height;
				const int  along0  = vertical ? box.y : box.x;
				const int  along1  = vertical ? box.y + box.height : box.x + box.width;
				const auto gray    = [&]( int along, int across ) {
                    const int         x  = vertical ? across : along;
                    const int         y  = vertical ? along : across;
                    const std::size_t at = ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( image.width ) ) + static_cast<std::size_t>( x ) ) * 3;
                    return ( ( image.rgb[at] * 299 ) + ( image.rgb[at + 1] * 587 ) + ( image.rgb[at + 2] * 114 ) ) / 1000;
				};
				if ( along1 - along0 < 2 || across1 - across0 < 2 )
				{
					return box;
				}
				// The background is the median of the margin's outermost lines.
				std::vector<int> edge;
				for ( int a = along0; a < along1; ++a )
				{
					edge.push_back( gray( a, across0 ) );
					edge.push_back( gray( a, across1 - 1 ) );
				}
				const auto middle = edge.begin() + static_cast<std::ptrdiff_t>( edge.size() / 2 );
				std::ranges::nth_element( edge, middle );
				const int  background = *middle;
				const auto inky       = [&]( int across ) {
                    int count = 0;
                    for ( int a = along0; a < along1; ++a )
                    {
                        count += std::abs( gray( a, across ) - background ) > 48 ? 1 : 0;
                    }
                    return count * 100 > along1 - along0;
				};
				int from = std::max( across0, vertical ? box.inner_x : box.inner_y );
				int to   = std::min( across1, from + ( vertical ? box.inner_width : box.inner_height ) );
				while ( from > across0 && inky( from - 1 ) )
				{
					--from;
				}
				while ( to < across1 && inky( to ) )
				{
					++to;
				}
				( vertical ? box.inner_x : box.inner_y )          = from;
				( vertical ? box.inner_width : box.inner_height ) = to - from;
				return box;
			}

			// The recognised text from the character under the pointer on, if the recognition is sure enough.
			[[nodiscard]] static std::optional<CapturedText> selectLine( const ocr::TextLine& line, const Rect& region, Point local, std::size_t max_chars, float gate )
			{
				const auto& chars = line.characters;
				if ( chars.empty() )
				{
					return std::nullopt;
				}
				const auto  along = static_cast<float>( line.vertical ? local.y : local.x );
				std::size_t start = 0;
				float       best  = std::numeric_limits<float>::max();
				for ( std::size_t i = 0; i < chars.size(); ++i )
				{
					const float distance = std::max( { 0.0F, chars[i].from - along, along - chars[i].to } );
					if ( distance < best )
					{
						best  = distance;
						start = i;
					}
				}
				if ( best > chars[start].to - chars[start].from )
				{
					return std::nullopt;
				}

				// Pictures and noise read as text come with low confidence.
				float       sum   = 0.0F;
				std::size_t count = 0;
				for ( std::size_t i = start; i < std::min( chars.size(), start + 4 ); ++i )
				{
					sum += chars[i].confidence;
					++count;
				}
				const float confidence = sum / static_cast<float>( count );
				if ( confidence < gate || chars[start].confidence < gate - 0.1F )
				{
					return std::nullopt;
				}

				const auto& b      = line.box;
				auto        handle = std::make_shared<OcrLine>();
				std::string text;
				std::string sentence;
				std::size_t sentence_offset = 0;
				for ( std::size_t i = 0; i < chars.size(); ++i )
				{
					if ( i == start )
					{
						sentence_offset = sentence.size();
					}
					sentence.append( chars[i].text );
					if ( i < start || handle->boxes.size() >= max_chars )
					{
						continue;
					}
					text.append( chars[i].text );
					const int from = static_cast<int>( std::lround( chars[i].from ) );
					const int to   = std::max( from + 1, static_cast<int>( std::lround( chars[i].to ) ) );
					handle->boxes.push_back( line.vertical ? Rect{ .x = region.x + b.inner_x, .y = region.y + from, .width = b.inner_width, .height = to - from } : Rect{ .x = region.x + from, .y = region.y + b.inner_y, .width = to - from, .height = b.inner_height } );
				}
				CapturedText captured{ .text = std::move( text ), .offset = 0, .character = handle->boxes.front() };
				captured.handle          = std::move( handle );
				captured.sentence        = std::move( sentence );
				captured.sentence_offset = sentence_offset;
				captured.confidence      = confidence * 100.0F;
				return captured;
			}

			[[nodiscard]] std::optional<Rect> windowRect( std::uint64_t id ) const
			{
				return id == 0 ? std::nullopt : screen_->windowRect( id );
			}

			// A strip around the pointer along the reading direction, anchored to a grid so that moving along a line
			// keeps hitting the recognition cache, and clipped to the window under the pointer.
			[[nodiscard]] Rect regionAround( Point point, const WindowInfo& window ) const
			{
				const int u = unit_;
				Rect      region;
				if ( vertical_ )
				{
					region = { .x = floorTo( point.x - ( 5 * u / 2 ), u ), .y = floorTo( point.y - ( 10 * u ), 2 * u ), .width = 5 * u, .height = 40 * u };
				}
				else
				{
					region = { .x = floorTo( point.x - ( 10 * u ), 2 * u ), .y = floorTo( point.y - ( 5 * u / 2 ), u ), .width = 40 * u, .height = 5 * u };
				}

				region = intersect( region, screen_->bounds() );
				if ( const auto bounds = windowRect( window.id ) )
				{
					region = intersect( region, *bounds );
				}
				return region;
			}

			Grab grabAround( Point point, const WindowInfo& window )
			{
				return grabRegion( regionAround( point, window ), point );
			}

			Grab grabRegion( const Rect& region, Point point )
			{
				Grab grab;
				grab.region = region;
				if ( grab.region.width < 8 || grab.region.height < 8 || !grab.region.contains( point ) )
				{
					return grab;
				}
				grab.pointer = { .x = point.x - grab.region.x, .y = point.y - grab.region.y };

				auto rgb = screen_->read( grab.region );
				if ( rgb.size() != static_cast<std::size_t>( grab.region.width ) * static_cast<std::size_t>( grab.region.height ) * 3 )
				{
					return grab;
				}
				grab.rgb = std::move( rgb );
				grab.gray.resize( grab.rgb.size() / 3 );
				for ( std::size_t i = 0; i < grab.gray.size(); ++i )
				{
					const int r  = grab.rgb[i * 3];
					const int g  = grab.rgb[( i * 3 ) + 1];
					const int b  = grab.rgb[( i * 3 ) + 2];
					grab.gray[i] = static_cast<std::uint8_t>( ( ( r * 299 ) + ( g * 587 ) + ( b * 114 ) ) / 1000 );
				}

				if ( overlays_ )
				{
					mask( grab, liveOverlays() );
				}
				return grab;
			}

			// Recognising a line takes tens of milliseconds; identical pixels are recognised only once.
			const Recognition& recognition( const Grab& grab, const Tone& tone, int upscale, std::pair<int, int> core )
			{
				const std::uint64_t key = grab.hash ^ ( tone.dark ? 0x5bd1e995ULL : 0ULL );
				for ( const Recognition& entry : cache_ )
				{
					if ( entry.hash == key && entry.region.x == grab.region.x && entry.region.y == grab.region.y && entry.region.width == grab.region.width && entry.region.height == grab.region.height )
					{
						return entry;
					}
				}

				int  width  = 0;
				int  height = 0;
				auto input  = prepare( grab, tone, upscale, width, height );
				auto found  = engine_->recognize( input, width, height );
				for ( OcrSymbol& symbol : found )
				{
					symbol.box = {
						.x      = grab.region.x + ( ( symbol.box.x - margin ) / upscale ),
						.y      = grab.region.y + ( ( symbol.box.y - margin ) / upscale ),
						.width  = std::max( 1, symbol.box.width / upscale ),
						.height = std::max( 1, symbol.box.height / upscale ),
					};
				}
				const bool boxless = std::ranges::any_of( found, [&]( const OcrSymbol& symbol ) {
					return !isSpace( symbol.text ) && ( symbol.box.width < 2 || symbol.box.height < 2 || intersect( symbol.box, grab.region ).empty() );
				} );
				if ( !boxless )
				{
					// The LSTM occasionally reports a character over blank background; such phantoms would shift every
					// following character.
					const int threshold = ( tone.ink + tone.background ) / 2;
					std::erase_if( found, [&]( const OcrSymbol& symbol ) {
						const Rect whole = intersect( symbol.box, grab.region );
						// Edges are ignored: they catch the anti-aliasing of neighbouring glyphs.
						const int  trim_x = vertical_ ? 0 : whole.width / 5;
						const int  trim_y = vertical_ ? whole.height / 5 : 0;
						const Rect box{ .x = whole.x + trim_x, .y = whole.y + trim_y, .width = whole.width - ( 2 * trim_x ), .height = whole.height - ( 2 * trim_y ) };
						if ( isSpace( symbol.text ) || box.empty() )
						{
							return false;
						}
						int ink = 0;
						for ( int y = box.y; y < box.y + box.height; ++y )
						{
							for ( int x = box.x; x < box.x + box.width; ++x )
							{
								ink += tone.level( grab.at( x - grab.region.x, y - grab.region.y ) ) < threshold ? 1 : 0;
							}
						}
						return ink * 50 < box.width * box.height;
					} );
				}
				found = place( found, grab, tone, vertical_, !boxless, core );
				log::debug( "ocr: {} symbols in {}x{} at {},{} (x{}, {} background)", found.size(), grab.region.width, grab.region.height, grab.region.x, grab.region.y, upscale, tone.dark ? "dark" : "light" );

				Recognition& slot = cache_[next_slot_];
				next_slot_        = ( next_slot_ + 1 ) % cache_.size();
				slot              = { .region = grab.region, .hash = key, .symbols = std::move( found ) };
				return slot;
			}

			[[nodiscard]] std::pair<int, int> along( const Rect& box ) const
			{
				return vertical_ ? std::pair{ box.y, box.y + box.height } : std::pair{ box.x, box.x + box.width };
			}

			[[nodiscard]] std::pair<int, int> across( const Rect& box ) const
			{
				return vertical_ ? std::pair{ box.x, box.x + box.width } : std::pair{ box.y, box.y + box.height };
			}

			// Text of the pointer's line, starting at the character under the pointer.
			[[nodiscard]] std::optional<CapturedText> select( const Recognition& recognition, Point point, std::size_t max_chars ) const
			{
				const auto& symbols  = recognition.symbols;
				const int   cross    = vertical_ ? point.x : point.y;
				const int   position = vertical_ ? point.y : point.x;

				// The line nearest to the pointer across the reading direction.
				int line    = -1;
				int nearest = ( unit_ / 2 ) + 1;
				for ( const OcrSymbol& symbol : symbols )
				{
					if ( isSpace( symbol.text ) )
					{
						continue;
					}
					const auto [from, to] = across( symbol.box );
					const int distance    = std::max( { 0, from - cross, cross - to } );
					if ( distance < nearest )
					{
						nearest = distance;
						line    = symbol.line;
					}
				}
				if ( line < 0 )
				{
					log::debug( "ocr: no line near {},{}: {}", point.x, point.y, dumpSymbols( symbols ) );
					return std::nullopt;
				}

				std::vector<const OcrSymbol*> row;
				for ( const OcrSymbol& symbol : symbols )
				{
					if ( symbol.line == line && !isSpace( symbol.text ) )
					{
						row.push_back( &symbol );
					}
				}
				std::ranges::sort( row, {}, [&]( const OcrSymbol* s ) { return along( s->box ).first; } );

				std::vector<int> sizes;
				sizes.reserve( row.size() );
				for ( const OcrSymbol* s : row )
				{
					const auto [from, to] = along( s->box );
					sizes.push_back( to - from );
				}
				std::ranges::nth_element( sizes, sizes.begin() + static_cast<std::ptrdiff_t>( sizes.size() / 2 ) );
				const int median = std::max( 1, sizes[sizes.size() / 2] );

				// LSTM symbol boxes are often offset or merged across characters; unless all of them look sound, an even split of the
				// line locates the pointer more reliably.
				std::size_t odd = 0;
				for ( std::size_t i = 0; i < row.size(); ++i )
				{
					const auto [from, to] = along( row[i]->box );
					bool bad              = to - from > ( median * 17 ) / 10;
					if ( i > 0 )
					{
						const auto [previous_from, previous_to] = along( row[i - 1]->box );
						bad                                     = bad || ( ( from + to ) / 2 ) - ( ( previous_from + previous_to ) / 2 ) < median * 4 / 10;
					}
					odd += bad ? 1 : 0;
				}
				const bool plausible = odd == 0;

				const auto [line_from, line_to] = std::pair{ along( row.front()->box ).first, along( row.back()->box ).second };
				if ( position < line_from - ( median / 2 ) || position > line_to + ( median / 2 ) )
				{
					log::debug( "ocr: {},{} is beside the line: {}", point.x, point.y, dumpSymbols( symbols ) );
					return std::nullopt;
				}

				std::vector<Rect> boxes;
				boxes.reserve( row.size() );
				if ( plausible )
				{
					for ( const OcrSymbol* s : row )
					{
						boxes.push_back( s->box );
					}
				}
				else
				{
					int low  = std::numeric_limits<int>::max();
					int high = std::numeric_limits<int>::min();
					for ( const OcrSymbol* s : row )
					{
						const auto [from, to] = across( s->box );
						low                   = std::min( low, from );
						high                  = std::max( high, to );
					}
					const double cell = static_cast<double>( line_to - line_from ) / static_cast<double>( row.size() );
					for ( std::size_t i = 0; i < row.size(); ++i )
					{
						const int from = line_from + static_cast<int>( std::lround( cell * static_cast<double>( i ) ) );
						const int to   = line_from + static_cast<int>( std::lround( cell * static_cast<double>( i + 1 ) ) );
						boxes.push_back( vertical_ ? Rect{ .x = low, .y = from, .width = high - low, .height = to - from } : Rect{ .x = from, .y = low, .width = to - from, .height = high - low } );
					}
				}

				// The character whose centre is nearest to the pointer.
				std::size_t start = 0;
				int         best  = std::numeric_limits<int>::max();
				for ( std::size_t i = 0; i < boxes.size(); ++i )
				{
					const auto [from, to] = along( boxes[i] );
					const int distance    = std::abs( ( ( from + to ) / 2 ) - position );
					if ( distance < best )
					{
						best  = distance;
						start = i;
					}
				}

				auto        handle = std::make_shared<OcrLine>();
				std::string text;
				int         previous_end = along( boxes[start] ).first;
				for ( std::size_t i = start; i < row.size() && handle->boxes.size() < max_chars; ++i )
				{
					const auto [from, to] = along( boxes[i] );
					// A wide gap ends the phrase (the next column of a table, a separate label).
					if ( !handle->boxes.empty() && from - previous_end > 2 * median )
					{
						break;
					}
					text.append( row[i]->text );
					handle->boxes.push_back( boxes[i] );
					previous_end = to;
				}

				if ( log::enabled( log::Level::Debug ) )
				{
					std::string recognised;
					for ( const OcrSymbol* s : row )
					{
						recognised.append( s->text );
					}
					log::debug( "ocr: line «{}» ({} boxes), reading from #{}: «{}»", recognised, plausible ? "character" : "evenly split", start, text );
				}

				std::string sentence;
				std::size_t sentence_offset = 0;
				for ( std::size_t i = 0; i < row.size(); ++i )
				{
					if ( i == start )
					{
						sentence_offset = sentence.size();
					}
					sentence.append( row[i]->text );
				}

				// How sure Tesseract was of what is looked up; marks recovered from the ink carry no confidence of their own.
				float confidence = 0.0F;
				int   counted    = 0;
				for ( std::size_t i = start; i < std::min( row.size(), start + handle->boxes.size() ); ++i )
				{
					if ( row[i]->confidence > 0.0F || !isSmallPunctuation( row[i]->text ) )
					{
						confidence += row[i]->confidence;
						++counted;
					}
				}

				CapturedText captured{ .text = std::move( text ), .offset = 0, .character = handle->boxes.front() };
				captured.handle          = std::move( handle );
				captured.confidence      = counted > 0 ? confidence / static_cast<float>( counted ) : 0.0F;
				captured.sentence        = std::move( sentence );
				captured.sentence_offset = sentence_offset;
				return captured;
			}

			std::unique_ptr<ScreenReader>         screen_;
			std::unique_ptr<TesseractEngine>      engine_;
			std::unique_ptr<ocr::PaddleOcr>       paddle_;
			std::vector<PaddleRegion>             paddle_cache_;
			std::function<std::vector<Overlay>()> overlays_;
			bool                                  vertical_;
			int                                   unit_;
			std::vector<const lang::Language*>    languages_;
			std::string                           description_;
			std::array<Recognition, 4>            cache_{};
			std::size_t                           next_slot_ = 0;
		};

	} // namespace

	Result<std::unique_ptr<TextCapture>> createOcrCapture( const OcrOptions& options )
	{
		// PaddleOCR reads stylised, outlined and vertical text far better; Tesseract is the fallback.
		std::unique_ptr<ocr::PaddleOcr> paddle;
		if ( options.engine != config::OcrEngine::Tesseract )
		{
			auto loaded = ocr::PaddleOcr::load( paths::ocrDir() / "paddle", paths::ocrDir() / "runtime", 4, options.languages );
			if ( loaded )
			{
				paddle = std::move( *loaded );
				// Load the networks now instead of on the first scan.
				const ocr::Image blank{ .width = 64, .height = 32, .rgb = std::vector<std::uint8_t>( 64UL * 32UL * 3UL, 255 ) };
				( void )paddle->detect( blank );
				( void )paddle->recognize( blank, { .x = 0, .y = 0, .width = 64, .height = 32, .score = 1.0F } );
			}
			else if ( options.engine == config::OcrEngine::Paddle )
			{
				return std::unexpected( loaded.error() );
			}
			else
			{
				log::debug( "ocr: PaddleOCR unavailable ({}), using Tesseract", loaded.error().message );
			}
		}

		// The languages read: PaddleOCR's default recogniser reads those without a recogniser of their own. Tesseract
		// reads those whose models are installed next to the first one found (vertical text: Japanese only).
		const auto                         wanted = options.languages.empty() ? std::vector( lang::languages().begin(), lang::languages().end() ) : options.languages;
		std::vector<const lang::Language*> languages;
		std::string                        models;
		if ( paddle )
		{
			const auto own = paddle->languages();
			std::ranges::copy_if( wanted, std::back_inserter( languages ), [&]( const lang::Language* language ) {
				return std::ranges::contains( own, language->code() ) || ( language->ocrModels().paddle.empty() && paddle->hasDefaultRecognizer() );
			} );
		}

		std::unique_ptr<TesseractEngine> tesseract;
		if ( !paddle )
		{
			std::optional<std::filesystem::path> datapath;
			for ( const lang::Language* language : wanted )
			{
				const auto             names = language->ocrModels();
				const std::string_view name  = options.vertical ? names.tesseract_vertical : names.tesseract;
				const auto             found = name.empty() ? std::nullopt : findTessdata( name, options.model );
				if ( !found || ( datapath && *found != *datapath ) )
				{
					continue;
				}
				datapath = found;
				models.append( models.empty() ? "" : "+" ).append( name );
				languages.push_back( language );
			}
			if ( !datapath )
			{
				return fail( "no OCR model is installed" );
			}
			auto engine = TesseractEngine::load( *datapath, models, options.vertical );
			if ( !engine )
			{
				return std::unexpected( engine.error() );
			}
			const std::vector<std::uint8_t> blank( 64UL * 32UL, 255 );
			( void )( *engine )->recognize( blank, 64, 32 );
			tesseract = std::move( *engine );
		}

		if ( !options.screen )
		{
			return fail( "this desktop cannot read the screen" );
		}
		auto screen = options.screen();
		if ( !screen )
		{
			return std::unexpected( screen.error() );
		}
		return std::make_unique<OcrCapture>( std::move( *screen ), std::move( tesseract ), std::move( paddle ), options, models, std::move( languages ) );
	}

} // namespace lexiglance::platform
