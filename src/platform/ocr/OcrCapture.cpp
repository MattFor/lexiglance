#include "OcrCapture.h"

#include "Tesseract.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/ocr/Onnx.h>
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
#include <numeric>
#include <ranges>
#include <span>
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
			// Where the crop would start at a gap as wide as a space rather than between two glyphs (along the strip): in
			// scripts written with spaces, the start of the word the crop cuts into.
			int word_begin = 0;
		};

		struct Recognition
		{
			Rect                   region;
			std::uint64_t          hash = 0;
			std::vector<OcrSymbol> symbols;
			// Placed as words of a script written with spaces, the spaces included.
			bool words = false;
		};

		// The line read, a box for each of its code points; kept alive as the capture handle for bounds().
		struct OcrLine
		{
			std::vector<Rect> boxes;
			// The character under the pointer, where the captured text starts.
			std::size_t first = 0;
			// Where the lines a sentence runs on to begin, as indices into `boxes` (the first line begins at 0).
			std::vector<std::size_t> line_starts;
		};

		// Whether the text of a line ends a sentence: with 。！？…. or ! ? (closing brackets and quotes after them count).
		bool endsSentence( std::string_view text )
		{
			std::u32string characters = utf8::toUtf32( text );
			while ( !characters.empty() && ( characters.back() == U' ' || std::u32string_view( U"」』）)\"'»”’】〕" ).contains( characters.back() ) ) )
			{
				characters.pop_back();
			}
			return characters.empty() || std::u32string_view( U"。．！？!?…‥." ).contains( characters.back() );
		}

		// Characters set on a square grid: Chinese characters, kana, their punctuation and full-width forms.
		bool gridCharacter( char32_t c )
		{
			return ( c >= 0x3000 && c <= 0x30FF ) || ( c >= 0x31F0 && c <= 0x31FF ) || ( c >= 0x3400 && c <= 0x4DBF ) || ( c >= 0x4E00 && c <= 0x9FFF ) || ( c >= 0xF900 && c <= 0xFAFF ) ||
			       ( c >= 0xFF00 && c <= 0xFFEF ) || ( c >= 0x20000 && c <= 0x3FFFF );
		}

		// Whether a line is mostly letters of scripts written with spaces between words (Cyrillic, Greek, Hangul, Latin)
		// rather than Japanese, which any kana gives away (Windowsで is Japanese). Digits, punctuation and spaces count for
		// neither.
		template <typename Texts>
		bool wordScript( const Texts& texts )
		{
			int grid    = 0;
			int letters = 0;
			for ( const std::string_view text : texts )
			{
				const char32_t c = utf8::first( text );
				if ( ( c >= 0x3040 && c <= 0x30FF ) || ( c >= 0x31F0 && c <= 0x31FF ) || ( c >= 0xFF66 && c <= 0xFF9D ) )
				{
					return false;
				}
				if ( gridCharacter( c ) )
				{
					++grid;
				}
				else if ( ( c >= U'A' && c <= U'Z' ) || ( c >= U'a' && c <= U'z' ) || ( c >= 0xC0 && c != 0xD7 && c != 0xF7 && ( c < 0x2000 || c > 0x2BFF ) ) )
				{
					++letters;
				}
			}
			return letters > grid;
		}

		// The script most of a line's letters are in, roughly: Japanese (kana and kanji), Latin, Cyrillic, Greek, Hangul;
		// 0 when it has no letters. A line in another script than the one before it does not go on with its sentence.
		int mainScript( const ocr::TextLine& line )
		{
			std::array<int, 6> counts{};
			for ( const ocr::Character& character : line.characters )
			{
				const char32_t c     = utf8::first( character.text );
				const auto     count = [&]( std::size_t script ) { ++counts[script]; };
				if ( ( c >= 0x3040 && c <= 0x30FF ) || ( c >= 0x3400 && c <= 0x4DBF ) || ( c >= 0x4E00 && c <= 0x9FFF ) || ( c >= 0xFF66 && c <= 0xFF9D ) )
				{
					count( 1 );
				}
				else if ( ( c >= U'A' && c <= U'Z' ) || ( c >= U'a' && c <= U'z' ) || ( c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7 ) )
				{
					count( 2 );
				}
				else if ( c >= 0x400 && c <= 0x52F )
				{
					count( 3 );
				}
				else if ( c >= 0x370 && c <= 0x3FF )
				{
					count( 4 );
				}
				else if ( ( c >= 0xAC00 && c <= 0xD7A3 ) || ( c >= 0x1100 && c <= 0x11FF ) || ( c >= 0x3130 && c <= 0x318F ) )
				{
					count( 5 );
				}
			}
			const auto* const most = std::ranges::max_element( counts );
			return *most > 0 ? static_cast<int>( most - counts.begin() ) : 0;
		}

		// Marks around words in scripts written with spaces: . , ! ? : ; quotation marks, brackets, dashes.
		bool isPunctuation( char32_t c )
		{
			if ( c < 0x80 )
			{
				return c > U' ' && ( c < U'0' || c > U'9' ) && ( c < U'A' || c > U'Z' ) && ( c < U'a' || c > U'z' );
			}
			return c == 0xA1 || c == 0xAB || c == 0xBB || c == 0xBF || ( c >= 0x2010 && c <= 0x205E );
		}

		// Punctuation at either end of a word (the comma after it, the quotation mark before it) stands on ink of its own,
		// which recognition places only roughly: pointing at the last letter would find the comma. Each such mark gets
		// its own blob of ink, and the letter beside it everything up to the gap. `edges` are the boundaries of the
		// word's characters along the line (edges[0] its start, edges.back() its end), `inked` tells a column with ink.
		template <typename Inked>
		void separatePunctuation( std::vector<int>& edges, const std::vector<std::string_view>& texts, int height, const Inked& inked )
		{
			const std::size_t n = texts.size();
			if ( n < 2 || edges.size() != n + 1 )
			{
				return;
			}
			const auto  mark  = [&]( std::size_t i ) { return isPunctuation( utf8::first( texts[i] ) ); };
			const int   begin = edges.front();
			const int   end   = edges.back();
			std::size_t last  = n;
			while ( last > 1 && mark( last - 1 ) )
			{
				const std::size_t k  = last - 1;
				int               r1 = edges[k + 1];
				while ( r1 > begin && !inked( r1 - 1 ) )
				{
					--r1;
				}
				int r0 = r1;
				while ( r0 > begin && inked( r0 - 1 ) )
				{
					--r0;
				}
				int gap = r0;
				while ( gap > begin && !inked( gap - 1 ) )
				{
					--gap;
				}
				// A narrow blob after a gap, near where the mark was read, with room left for the letters before it.
				if ( r0 >= r1 || gap >= r0 || r1 - r0 > ( height * 3 ) / 5 || r0 < edges[k] - height || gap <= edges[k - 1] )
				{
					break;
				}
				edges[k] = gap;
				last     = k;
			}
			std::size_t first = 0;
			while ( first + 1 < last && mark( first ) )
			{
				const std::size_t k  = first;
				int               r0 = edges[k];
				while ( r0 < end && !inked( r0 ) )
				{
					++r0;
				}
				int r1 = r0;
				while ( r1 < end && inked( r1 ) )
				{
					++r1;
				}
				int gap = r1;
				while ( gap < end && !inked( gap ) )
				{
					++gap;
				}
				if ( r0 >= r1 || gap <= r1 || r1 - r0 > ( height * 3 ) / 5 || r1 > edges[k + 1] + height || gap >= edges[k + 2] )
				{
					break;
				}
				edges[k + 1] = gap;
				first        = k + 1;
			}
		}

		// Appends a box for each code point of `text` (a symbol can be several: a letter and its accent).
		void addBoxes( std::vector<Rect>& boxes, std::string_view text, const Rect& box )
		{
			boxes.insert( boxes.end(), std::max<std::size_t>( 1, utf8::length( text ) ), box );
		}

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

		// The narrowest blank gap that is a space between words, from the gaps of a line: its gaps fall into those between
		// letters and those between words, split where the width jumps most (by half at least). None when they are all
		// alike.
		std::optional<int> spaceSplit( std::vector<int> gaps, int height )
		{
			std::ranges::sort( gaps );
			const int least = std::max( 3, ( height + 4 ) / 5 );
			int       split = 0;
			double    jump  = 1.5;
			for ( std::size_t i = 1; i < gaps.size(); ++i )
			{
				const int a = gaps[i - 1];
				const int b = gaps[i];
				if ( b < least || b <= a )
				{
					continue;
				}
				// Of equal jumps, the one between wider gaps: spaces are the widest.
				if ( const double ratio = static_cast<double>( b ) / std::max( a, 1 ); ratio >= jump )
				{
					jump  = ratio;
					split = ( a + b + 1 ) / 2;
				}
			}
			return split > 0 ? std::optional<int>( split ) : std::nullopt;
		}

		bool isHangul( char32_t c )
		{
			return ( c >= 0xAC00 && c <= 0xD7A3 ) || ( c >= 0x1100 && c <= 0x11FF ) || ( c >= 0x3130 && c <= 0x318F );
		}

		bool isLatinLetter( char32_t c )
		{
			return ( c >= U'A' && c <= U'Z' ) || ( c >= U'a' && c <= U'z' ) || c == 0xB5 || ( c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7 );
		}

		// Latin letters, and the micro sign, that look exactly like a Cyrillic or a Greek letter.
		struct Homoglyph
		{
			char32_t latin;
			char32_t cyrillic;
			char32_t greek;
		};

		constexpr std::array homoglyphs = std::to_array<Homoglyph>( {
				{ U'A', U'А', U'Α' },
				{ U'B', U'В', U'Β' },
				{ U'C', U'С', 0 },
				{ U'E', U'Е', U'Ε' },
				{ U'H', U'Н', U'Η' },
				{ U'I', U'І', U'Ι' },
				{ U'K', U'К', U'Κ' },
				{ U'M', U'М', U'Μ' },
				{ U'N', 0, U'Ν' },
				{ U'O', U'О', U'Ο' },
				{ U'P', U'Р', U'Ρ' },
				{ U'T', U'Т', U'Τ' },
				{ U'X', U'Х', U'Χ' },
				{ U'Y', U'У', U'Υ' },
				{ U'Z', 0, U'Ζ' },
				{ U'a', U'а', 0 },
				{ U'c', U'с', 0 },
				{ U'e', U'е', 0 },
				{ U'i', U'і', 0 },
				{ U'o', U'о', U'ο' },
				{ U'p', U'р', 0 },
				{ U'u', 0, U'υ' },
				{ U'v', 0, U'ν' },
				{ U'x', U'х', 0 },
				{ U'y', U'у', 0 },
				{ U'Ë', U'Ё', 0 },
				{ U'ë', U'ё', 0 },
				{ U'µ', 0, U'μ' },
		} );

		// Recognisers that read several scripts mix up letters that look the same in them: Latin o for Cyrillic о, the
		// micro sign for μ, a lone в as B. A word written in Cyrillic or Greek, or made only of such look-alikes in a line
		// that is, gets its Latin look-alikes back in that script; a word with Latin letters that look like nothing there
		// is Latin (iPhone).
		template <typename Item>
		void mendHomoglyphs( std::vector<Item>& items )
		{
			struct Count
			{
				int cyrillic = 0;
				int greek    = 0;
				int latin    = 0;
			};
			const auto count = [&]( std::size_t begin, std::size_t end ) {
				Count counted;
				for ( std::size_t i = begin; i < end; ++i )
				{
					const char32_t c = utf8::first( items[i].text );
					counted.cyrillic += c >= 0x0400 && c <= 0x052F ? 1 : 0;
					counted.greek += ( c >= 0x0370 && c <= 0x03FF ) || ( c >= 0x1F00 && c <= 0x1FFF ) ? 1 : 0;
					counted.latin += isLatinLetter( c ) ? 1 : 0;
				}
				return counted;
			};
			const Count line = count( 0, items.size() );

			const auto word = [&]( std::size_t begin, std::size_t end ) {
				const Count here = count( begin, end );
				if ( here.latin == 0 )
				{
					return;
				}
				// The word's own script, or the line's when it is written in one of them and has hardly any Latin.
				bool into_greek = here.greek > here.cyrillic;
				if ( here.cyrillic + here.greek == 0 )
				{
					into_greek = line.greek > line.cyrillic;
					if ( std::max( line.cyrillic, line.greek ) < 3 * line.latin )
					{
						return;
					}
				}
				const auto target = [&]( char32_t c ) -> char32_t {
					const auto* it = std::ranges::find( homoglyphs, c, &Homoglyph::latin );
					if ( it == homoglyphs.end() )
					{
						return 0;
					}
					return into_greek ? it->greek : it->cyrillic;
				};
				for ( std::size_t i = begin; i < end; ++i )
				{
					const char32_t c = utf8::first( items[i].text );
					if ( isLatinLetter( c ) && ( utf8::length( items[i].text ) != 1 || target( c ) == 0 ) )
					{
						return;
					}
				}
				for ( std::size_t i = begin; i < end; ++i )
				{
					if ( const char32_t to = target( utf8::first( items[i].text ) ); to != 0 && utf8::length( items[i].text ) == 1 )
					{
						items[i].text.clear();
						utf8::append( items[i].text, to );
					}
				}
			};
			std::size_t begin = 0;
			for ( std::size_t i = 0; i <= items.size(); ++i )
			{
				if ( i == items.size() || isSpace( items[i].text ) )
				{
					word( begin, i );
					begin = i + 1;
				}
			}
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

		// The screen area of `length` characters from the start of a capture: the handle's boxes from the one under the
		// pointer, less the characters put in front of it (the start of its word).
		// The same span a rectangle for each line it is on.
		std::vector<Rect> spansOf( const OcrLine& line, const CapturedText& text, std::size_t length )
		{
			const std::size_t from = line.first - std::min( line.first, text.rewound );
			const std::size_t to   = std::min( line.boxes.size(), from + std::min( length, utf8::length( text.text ) ) );
			std::vector<Rect> spans;
			for ( std::size_t i = from; i < to; ++i )
			{
				// A new line starts at this box, or the first box of the span.
				if ( i == from || std::ranges::contains( line.line_starts, i ) )
				{
					spans.push_back( line.boxes[i] );
					continue;
				}
				spans.back() = unite( spans.back(), line.boxes[i] );
			}
			std::erase_if( spans, []( const Rect& span ) { return span.empty(); } );
			return spans;
		}

		std::optional<Rect> spanOf( const OcrLine& line, const CapturedText& text, std::size_t length )
		{
			const std::size_t from = line.first - std::min( line.first, text.rewound );
			const std::size_t to   = std::min( line.boxes.size(), from + std::min( length, utf8::length( text.text ) ) );
			if ( from >= to )
			{
				return std::nullopt;
			}
			Rect area = line.boxes[from];
			for ( std::size_t i = from + 1; i < to; ++i )
			{
				area = unite( area, line.boxes[i] );
			}
			return area;
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

		struct Along
		{
			int begin      = 0;
			int end        = 0;
			int word_begin = 0;
		};

		// Along the line: a little before the pointer and plenty after, on a grid for the cache. Glyphs cut by the strip's
		// edges would be read as garbage, so they are left out; elsewhere the crop never cuts through a glyph.
		Along alongRange( const InkMask& mask, int origin, int pa, std::pair<int, int> clear, const Rows& rows )
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
			// Back from the start to the nearest gap as wide as a space, within the strip.
			int word_begin = begin;
			int run        = 0;
			for ( int a = begin - 1; a >= std::max( a0, begin - ( 12 * height ) ); --a )
			{
				run = blank( a ) ? run + 1 : 0;
				if ( run >= std::max( 2, ( height + 2 ) / 4 ) )
				{
					word_begin = a + run;
					break;
				}
			}
			return { .begin = begin, .end = end, .word_begin = word_begin };
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
			const auto [begin, end, word_begin] = alongRange( mask, vertical ? grab.region.y : grab.region.x, pa, { a0, a1 }, *rows );
			const int height                    = rows->hi - rows->lo + 1;

			Line line;
			line.upscale    = std::clamp( static_cast<int>( std::lround( target_height / height ) ), 1, 4 );
			line.crop       = vertical ? Rect{ .x = rows->from, .y = begin, .width = rows->to - rows->from + 1, .height = end - begin }
			                           : Rect{ .x = begin, .y = rows->from, .width = end - begin, .height = rows->to - rows->from + 1 };
			line.core_from  = rows->lo - rows->from;
			line.core_to    = rows->hi - rows->from + 1;
			line.word_begin = word_begin;
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

		// Scripts written with spaces are set in letters of their own widths, not on a grid: each letter stays where Tesseract
		// saw it, each word's edges go onto its ink (Tesseract's boxes stop a little short or run into the space), and a
		// space goes between words, as the lookup ends a word there.
		std::vector<OcrSymbol> placeWords( const std::vector<OcrSymbol>& symbols, const Grab& grab, const Tone& tone, std::pair<int, int> core )
		{
			const InkLine line( grab, tone, false, core.first, core.second );
			if ( line.empty() )
			{
				return {};
			}
			// Tesseract may also report strays of the lines above and below as lines of their own: the one on the core rows
			// is the pointer's.
			std::vector<int> hits;
			for ( const OcrSymbol& symbol : symbols )
			{
				const int middle = symbol.box.y + ( symbol.box.height / 2 ) - grab.region.y;
				if ( !isSpace( symbol.text ) && symbol.line >= 0 && middle >= core.first && middle < core.second )
				{
					hits.resize( std::max( hits.size(), static_cast<std::size_t>( symbol.line ) + 1 ) );
					++hits[static_cast<std::size_t>( symbol.line )];
				}
			}
			if ( hits.empty() )
			{
				return {};
			}
			const auto                    main = static_cast<int>( std::ranges::max_element( hits ) - hits.begin() );
			std::vector<const OcrSymbol*> letters;
			for ( const OcrSymbol& symbol : symbols )
			{
				if ( symbol.line == main && !isSpace( symbol.text ) )
				{
					letters.push_back( &symbol );
				}
			}

			// Words as ranges of letters, each with its extent along the line.
			struct Word
			{
				std::size_t begin = 0;
				std::size_t end   = 0;
				int         from  = 0;
				int         to    = 0;
			};
			std::vector<Word> words;
			for ( std::size_t i = 0; i < letters.size(); ++i )
			{
				const auto [from, to] = line.span( letters[i]->box );
				if ( words.empty() || letters[i]->word || from >= words.back().to + ( line.height() / 2 ) )
				{
					words.push_back( { .begin = i, .end = i + 1, .from = from, .to = to } );
					continue;
				}
				words.back().end  = i + 1;
				words.back().from = std::min( words.back().from, from );
				words.back().to   = std::max( words.back().to, to );
			}
			for ( std::size_t w = 0; w < words.size(); ++w )
			{
				Word&     word  = words[w];
				const int floor = w > 0 ? words[w - 1].to : 0;
				const int ceil  = w + 1 < words.size() ? words[w + 1].from : line.length();
				int       from  = std::clamp( word.from, floor, line.length() );
				int       to    = std::clamp( word.to, from, std::max( from, ceil ) );
				while ( from < to && line.blank( from ) )
				{
					++from;
				}
				while ( to > from && line.blank( to - 1 ) )
				{
					--to;
				}
				if ( from == to )
				{
					// No ink where Tesseract put the word: its own boxes are all there is.
					word.from = std::clamp( word.from, floor, line.length() );
					word.to   = std::clamp( word.to, word.from, line.length() );
					continue;
				}
				// Ink running on past the box is the rest of a glyph Tesseract cut short.
				while ( from > floor && !line.blank( from - 1 ) )
				{
					--from;
				}
				while ( to < ceil && !line.blank( to ) )
				{
					++to;
				}
				word.from = from;
				word.to   = to;
			}

			// Tesseract takes every Hangul syllable for a word: between two, only a gap as wide as a space is one.
			std::vector<int> gaps;
			for ( std::size_t w = 1; w < words.size(); ++w )
			{
				gaps.push_back( words[w].from - words[w - 1].to );
			}
			if ( log::enabled( log::Level::Debug ) )
			{
				std::string list;
				for ( const int g : gaps )
				{
					list += std::format( "{} ", g );
				}
				log::debug( "ocr: word gaps {}(height {})", list, line.height() );
			}
			// When the gaps are all alike, a quarter of the line's height tells.
			const int              space = spaceSplit( std::move( gaps ), line.height() ).value_or( std::max( 2, ( line.height() + 2 ) / 4 ) );
			std::vector<OcrSymbol> placed;
			for ( std::size_t w = 0; w < words.size(); ++w )
			{
				const Word& word = words[w];
				if ( w > 0 )
				{
					const int  gap      = words[w - 1].to;
					const bool syllable = isHangul( utf8::first( letters[word.begin - 1]->text ) ) && isHangul( utf8::first( letters[word.begin]->text ) );
					// In small text Tesseract also splits other words now and then, where their letters all but touch.
					const bool touching = line.height() <= 12 && word.from - gap <= 1;
					if ( syllable ? word.from - gap >= space : !touching )
					{
						placed.push_back( { .text = " ", .box = line.box( gap, std::max( word.from, gap + 1 ) ), .line = 0, .confidence = 0.0F, .word = false } );
					}
				}
				// Letters keep Tesseract's boundaries between them, stretched onto the word's ink and kept in order.
				const auto first = line.span( letters[word.begin]->box ).first;
				const auto last  = line.span( letters[word.end - 1]->box ).second;

				const auto onto = [&]( int at ) {
					if ( last <= first )
					{
						return word.from;
					}
					return word.from + static_cast<int>( std::lround( static_cast<double>( at - first ) * ( word.to - word.from ) / ( last - first ) ) );
				};
				std::vector<int>              edges{ word.from };
				std::vector<std::string_view> texts;
				for ( std::size_t i = word.begin; i < word.end; ++i )
				{
					const std::size_t remaining = word.end - i - 1;
					int               end       = word.to;
					if ( remaining > 0 )
					{
						const int boundary = ( line.span( letters[i]->box ).second + line.span( letters[i + 1]->box ).first ) / 2;
						end                = std::clamp( onto( boundary ), edges.back() + 1, std::max( edges.back() + 1, word.to - static_cast<int>( remaining ) ) );
					}
					edges.push_back( std::max( end, edges.back() + 1 ) );
					texts.emplace_back( letters[i]->text );
				}
				separatePunctuation( edges, texts, line.height(), [&]( int a ) { return !line.blank( a ); } );
				for ( std::size_t i = word.begin; i < word.end; ++i )
				{
					OcrSymbol symbol = *letters[i];
					symbol.box       = line.box( edges[i - word.begin], edges[i - word.begin + 1] );
					symbol.line      = 0;
					placed.push_back( std::move( symbol ) );
				}
			}
			mendHomoglyphs( placed );
			return placed;
		}

		// Which columns of a text box hold ink along its line: those with a pixel well off the background (the median of the
		// margin's outermost rows, as in inked()) on the line's own rows.
		struct InkColumns
		{
			int                       from = 0;
			std::vector<std::uint8_t> ink;
			// The topmost and bottommost inked row of each column, told from a plain background; empty when the ink had to
			// be found by its own colour, where the rows cannot be trusted.
			std::vector<int> top;
			std::vector<int> bottom;

			// How tall the ink is between two columns, or 0 when that cannot be told.
			[[nodiscard]] int height( int from_x, int to_x ) const noexcept
			{
				int highest = std::numeric_limits<int>::max();
				int lowest  = std::numeric_limits<int>::min();
				for ( int x = std::max( from_x, from ); x <= std::min( to_x, to() - 1 ); ++x )
				{
					const auto at = static_cast<std::size_t>( x - from );
					if ( at < top.size() && top[at] >= 0 )
					{
						highest = std::min( highest, top[at] );
						lowest  = std::max( lowest, bottom[at] );
					}
				}
				return lowest >= highest ? lowest - highest + 1 : 0;
			}

			[[nodiscard]] int to() const noexcept
			{
				return from + static_cast<int>( ink.size() );
			}

			[[nodiscard]] bool operator()( int x ) const noexcept
			{
				return x >= from && x < to() && ink[static_cast<std::size_t>( x - from )] != 0;
			}
		};

		InkColumns inkColumns( const ocr::Image& image, const ocr::Box& box, std::size_t letters )
		{
			InkColumns columns;
			const int  along0  = std::clamp( box.x, 0, image.width );
			const int  along1  = std::clamp( box.x + box.width, along0, image.width );
			const int  across0 = std::clamp( box.inner_y, 0, image.height );
			const int  across1 = std::clamp( box.inner_y + box.inner_height, across0, image.height );
			if ( along1 - along0 < 2 || across1 <= across0 || image.height < 1 )
			{
				return columns;
			}
			const int top    = std::clamp( box.y, 0, image.height - 1 );
			const int bottom = std::clamp( box.y + box.height - 1, 0, image.height - 1 );

			const auto gray = [&]( int x, int y ) {
				const std::size_t at = ( ( static_cast<std::size_t>( y ) * static_cast<std::size_t>( image.width ) ) + static_cast<std::size_t>( x ) ) * 3;
				return ( ( image.rgb[at] * 299 ) + ( image.rgb[at + 1] * 587 ) + ( image.rgb[at + 2] * 114 ) ) / 1000;
			};
			std::vector<int> edge;
			for ( int x = along0; x < along1; ++x )
			{
				edge.push_back( gray( x, top ) );
				edge.push_back( gray( x, bottom ) );
			}
			const auto inked_columns = [&]( const auto& inked ) {
				std::vector<std::uint8_t> ink( static_cast<std::size_t>( along1 - along0 ), 0 );
				for ( int x = along0; x < along1; ++x )
				{
					for ( int y = across0; y < across1; ++y )
					{
						if ( inked( gray( x, y ) ) )
						{
							ink[static_cast<std::size_t>( x - along0 )] = 1;
							break;
						}
					}
				}
				return ink;
			};
			const auto gaps = []( const std::vector<std::uint8_t>& ink ) {
				std::size_t count = 0;
				for ( std::size_t i = 1; i < ink.size(); ++i )
				{
					count += ink[i - 1] != 0 && ink[i] == 0 ? 1 : 0;
				}
				return count;
			};
			// Ink is whatever stands out of the background.
			std::ranges::sort( edge );
			const int background = edge[edge.size() / 2];
			columns.from         = along0;
			columns.ink          = inked_columns( [&]( int g ) { return std::abs( g - background ) > 48; } );
			// How tall that ink stands in each column, for telling one size of text from another.
			columns.top.assign( columns.ink.size(), -1 );
			columns.bottom.assign( columns.ink.size(), -1 );
			for ( int x = along0; x < along1; ++x )
			{
				const auto at = static_cast<std::size_t>( x - along0 );
				if ( columns.ink[at] == 0 )
				{
					continue;
				}
				for ( int y = across0; y < across1; ++y )
				{
					if ( std::abs( gray( x, y ) - background ) > 48 )
					{
						columns.top[at]    = columns.top[at] < 0 ? y : columns.top[at];
						columns.bottom[at] = y;
					}
				}
			}
			if ( gaps( columns.ink ) * 3 >= letters )
			{
				return columns;
			}
			// Unless that runs the letters together, over a busy background (a game's scene, stripes) or around outlined
			// letters: ink is then one of the letters' own colours (a gray that fills the box's middle much more than its
			// edges), their fill or their outline, whichever keeps them apart best.
			std::array<int, 16> border{};
			for ( const int g : edge )
			{
				++border[static_cast<std::size_t>( g / 16 )];
			}
			std::array<int, 256> inside{};
			int                  total = 0;
			for ( int x = along0; x < along1; ++x )
			{
				for ( int y = across0; y < across1; ++y )
				{
					++inside[static_cast<std::size_t>( gray( x, y ) )];
					++total;
				}
			}
			// Each colour as the commonest gray of its sixteen, with how much more of the middle it fills than of the edges.
			std::vector<std::pair<int, double>> colours;
			for ( std::size_t bin = 0; bin < border.size(); ++bin )
			{
				const auto   grays = std::span( inside ).subspan( bin * 16, 16 );
				const double lead  = ( static_cast<double>( std::accumulate( grays.begin(), grays.end(), 0 ) ) / total ) - ( static_cast<double>( border[bin] ) / static_cast<double>( edge.size() ) );
				if ( lead > 0.05 )
				{
					colours.emplace_back( static_cast<int>( ( bin * 16 ) + static_cast<std::size_t>( std::ranges::max_element( grays ) - grays.begin() ) ), lead );
				}
			}
			// Small plain letters on a plain background just stand close; their edges' grays are no colours of their own.
			const bool busy     = edge[( edge.size() * 3 ) / 4] - edge[edge.size() / 4] > 32;
			const bool outlined = std::ranges::any_of( colours, [&]( const auto& a ) {
				return a.second > 0.1 && std::ranges::any_of( colours, [&]( const auto& b ) { return b.second > 0.1 && std::abs( a.first - b.first ) >= 96; } );
			} );
			if ( !busy && !outlined )
			{
				return columns;
			}
			std::size_t most = gaps( columns.ink );
			for ( const int colour : colours | std::views::keys )
			{
				auto ink = inked_columns( [&]( int g ) { return std::abs( g - colour ) <= 16; } );
				if ( const auto apart = gaps( ink ); apart > most )
				{
					most        = apart;
					columns.ink = std::move( ink );
					// Found by colour: the rows above tell nothing about this ink.
					columns.top.clear();
					columns.bottom.clear();
				}
			}
			return columns;
		}

		// The blank runs between inked columns within [from, to), as [first, last) pairs; with `inner` only those with
		// ink on both sides.
		std::vector<std::pair<int, int>> blankRuns( const InkColumns& ink, int from, int to, bool inner )
		{
			std::vector<std::pair<int, int>> runs;
			for ( int x = from; x < to; )
			{
				if ( ink( x ) )
				{
					++x;
					continue;
				}
				int end = x;
				while ( end < to && !ink( end ) )
				{
					++end;
				}
				if ( !inner || ( x > from && end < to ) )
				{
					runs.emplace_back( x, end );
				}
				x = end;
			}
			return runs;
		}

		// Marks that close what comes before them, and marks that open what comes after: no space goes on their inner side.
		bool closes( char32_t c )
		{
			return std::u32string_view( U".,!?:;)]}»”’…" ).contains( c );
		}

		bool opens( char32_t c )
		{
			return std::u32string_view( U"([{«„“‘¿¡" ).contains( c );
		}

		// Small text sometimes comes without some of its spaces (Korean especially), which would run its words together: a
		// gap as wide as a space where none was read gets one. Only where the line's gaps clearly fall into two widths (in
		// small text letters can stand nearly as far apart as words), and nearly as wide as the spaces that were read. The
		// wide ones cannot be most of them: in a font whose letters stand apart (a game's, often) most gaps are wide, and
		// are between letters. With no space read at all, one wide gap is only a wide letter (as in a word the detection
		// boxed on its own) unless it is between Korean syllables.
		void recoverSpaces( std::vector<ocr::Character>& chars, const InkColumns& ink, int height )
		{
			const auto blanks = blankRuns( ink, ink.from, ink.to(), true );
			// The character after a gap, and whether a space was read beside it.
			const auto after = [&]( const std::pair<int, int>& blank ) {
				const float at = static_cast<float>( blank.first + blank.second ) / 2.0F;
				return std::ranges::find_if( chars, [&]( const ocr::Character& c ) { return ( c.from + c.to ) / 2.0F > at; } );
			};
			const auto beside_space = [&]( auto next ) { return next == chars.begin() || next == chars.end() || isSpace( next->text ) || isSpace( std::prev( next )->text ); };

			std::vector<int> gaps;
			std::vector<int> read;
			for ( const auto& blank : blanks )
			{
				gaps.push_back( blank.second - blank.first );
				if ( const auto next = after( blank ); next != chars.begin() && next != chars.end() && beside_space( next ) )
				{
					read.push_back( blank.second - blank.first );
				}
			}
			const auto split = spaceSplit( std::move( gaps ), height );
			if ( !split )
			{
				return;
			}
			int space = *split;
			if ( !read.empty() )
			{
				const auto usual = read.begin() + static_cast<std::ptrdiff_t>( read.size() / 2 );
				std::ranges::nth_element( read, usual );
				space = std::max( space, ( *usual * 3 ) / 4 );
			}
			const auto wide    = [&]( const std::pair<int, int>& blank ) { return blank.second - blank.first >= space; };
			const auto missing = [&]( const std::pair<int, int>& blank ) {
				const auto next = after( blank );
				return wide( blank ) && !beside_space( next ) && !closes( utf8::first( next->text ) ) && !opens( utf8::first( std::prev( next )->text ) );
			};
			// Korean is where one is left out most, even the only one of a line.
			const auto between_hangul = [&]( const std::pair<int, int>& blank ) {
				const auto next = after( blank );
				return isHangul( utf8::first( next->text ) ) && isHangul( utf8::first( std::prev( next )->text ) );
			};
			const auto wide_count    = std::ranges::count_if( blanks, wide );
			const auto missing_count = std::ranges::count_if( blanks, missing );
			if ( wide_count * 3 > std::ssize( blanks ) * 2 )
			{
				return;
			}
			// With no space read anywhere, what the line would read as decides: words of a letter or two throughout mean
			// the gaps were between letters, not words (a single word in large text stands wide apart). Where spaces were
			// read the line already shows where its words end, and short ones are ordinary («т. е. и т. д.»).
			const auto letters_in = std::ranges::count_if( chars, []( const ocr::Character& c ) { return !isSpace( c.text ); } );
			if ( read.empty() && letters_in * 2 < ( missing_count + 1 ) * 5 )
			{
				return;
			}
			const bool lone = read.empty() && missing_count < 2;
			for ( const auto& blank : blanks )
			{
				if ( !missing( blank ) || ( lone && !between_hangul( blank ) ) )
				{
					continue;
				}
				const auto  next       = after( blank );
				const float confidence = std::min( next->confidence, std::prev( next )->confidence );
				chars.insert( next, { .text = " ", .confidence = confidence, .from = static_cast<float>( blank.first ), .to = static_cast<float>( blank.second ) } );
			}
		}

		using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

		// Words as ranges of characters between spaces.
		Ranges wordRanges( const std::vector<ocr::Character>& chars )
		{
			Ranges words;
			for ( std::size_t i = 0; i < chars.size(); ++i )
			{
				if ( isSpace( chars[i].text ) )
				{
					continue;
				}
				if ( !words.empty() && words.back().second == i )
				{
					words.back().second = i + 1;
				}
				else
				{
					words.emplace_back( i, i + 1 );
				}
			}
			return words;
		}

		int centreOf( const ocr::Character& c )
		{
			return static_cast<int>( std::lround( ( c.from + c.to ) / 2.0F ) );
		}

		int widthOf( const ocr::Character& c )
		{
			return std::max( 1, static_cast<int>( std::lround( c.to - c.from ) ) );
		}

		// Japanese and Chinese are written without spaces, so a wide gap between two of their characters is not a word
		// break but a break between two separate things: the columns of a menu, a word beside its reading. It gets a
		// space, which ends the sentence there, so translating one of them leaves the other alone. Only an unmistakable
		// gap counts: kana are small in their squares, and in some fonts (Mincho, rounded ones) two of them stand more
		// than half a square apart with no space at all. Punctuation is skipped, filling a square of its own with a mark
		// in one corner.
		void spaceGaps( ocr::TextLine& line, const ocr::Image& image )
		{
			auto& chars = line.characters;
			if ( chars.size() < 2 )
			{
				return;
			}
			const InkColumns ink = inkColumns( image, line.box, chars.size() );
			if ( ink.ink.size() < 2 )
			{
				return;
			}
			const int  size  = std::max( 1, line.box.inner_height );
			const auto plain = [&]( const ocr::Character& c ) {
				const char32_t first = utf8::first( c.text );
				return !isSpace( c.text ) && first != 0x3000 && !isPunctuation( first ) && !isSmallPunctuation( c.text );
			};
			// The blank between two characters, at its widest.
			const auto blank = [&]( int from, int to ) {
				int widest = 0;
				int run    = 0;
				for ( int x = from; x <= to; ++x )
				{
					run    = ink( x ) ? 0 : run + 1;
					widest = std::max( widest, run );
				}
				return widest;
			};
			// A space of the reading's own: Japanese is read with 　, which a sentence may hold. One much wider than that
			// is not part of a sentence but a break between two things, and becomes a plain space, which ends it.
			for ( std::size_t i = 1; i + 1 < chars.size(); ++i )
			{
				if ( utf8::first( chars[i].text ) != 0x3000 )
				{
					continue;
				}
				if ( blank( centreOf( chars[i - 1] ), centreOf( chars[i + 1] ) ) * 5 >= size * 7 )
				{
					chars[i].text = " ";
				}
			}
			// How tall the letters stand around a place in the line, as the middle of the three nearest.
			const auto letters = [&]( std::size_t from, std::size_t to ) {
				std::vector<int> heights;
				for ( std::size_t i = from; i < to && i < chars.size(); ++i )
				{
					if ( const int tall = ink.height( static_cast<int>( chars[i].from ), static_cast<int>( chars[i].to ) ); plain( chars[i] ) && tall > 0 )
					{
						heights.push_back( tall );
					}
				}
				if ( heights.empty() )
				{
					return 0;
				}
				std::ranges::nth_element( heights, heights.begin() + static_cast<std::ptrdiff_t>( heights.size() / 2 ) );
				return heights[heights.size() / 2];
			};
			for ( std::size_t i = chars.size(); i-- > 1; )
			{
				if ( !plain( chars[i] ) || !plain( chars[i - 1] ) )
				{
					continue;
				}
				const int gap = blank( centreOf( chars[i - 1] ), centreOf( chars[i] ) );
				// Letters of a plainly different size beside these ones are something else: a word next to its reading, a
				// name beside what is said. They are parted where they meet, as long as they do not touch.
				const int before = letters( i > 3 ? i - 3 : 0, i );
				const int after  = letters( i, i + 3 );
				if ( gap * 8 >= size && before > 0 && after > 0 && std::min( before, after ) * 5 <= std::max( before, after ) * 3 )
				{
					chars.insert( chars.begin() + static_cast<std::ptrdiff_t>( i ), { .text = " ", .confidence = std::min( chars[i].confidence, chars[i - 1].confidence ), .from = chars[i - 1].to, .to = chars[i].from } );
					continue;
				}
				if ( gap * 5 >= size * 4 )
				{
					chars.insert( chars.begin() + static_cast<std::ptrdiff_t>( i ), { .text = " ", .confidence = std::min( chars[i].confidence, chars[i - 1].confidence ), .from = chars[i - 1].to, .to = chars[i].from } );
				}
			}
		}

		// The line's ends: its first and last inked columns near where its first and last letters were read.
		void lineEnds( const std::vector<ocr::Character>& chars, const Ranges& words, std::vector<std::pair<float, float>>& extents, const InkColumns& ink )
		{
			const ocr::Character& first = chars[words.front().first];
			for ( int x = centreOf( first ) - widthOf( first ); x <= centreOf( first ) + ( widthOf( first ) / 2 ); ++x )
			{
				if ( ink( x ) )
				{
					int from = x;
					while ( ink( from - 1 ) )
					{
						--from;
					}
					extents.front().first = static_cast<float>( from );
					break;
				}
			}
			const ocr::Character& last = chars[words.back().second - 1];
			for ( int x = static_cast<int>( std::lround( last.to ) ) + widthOf( last ); x >= centreOf( last ) - ( widthOf( last ) / 2 ); --x )
			{
				if ( ink( x ) )
				{
					int to = x + 1;
					while ( ink( to ) )
					{
						++to;
					}
					extents.back().second = static_cast<float>( to );
					break;
				}
			}
		}

		// The gap between two words: the widest from the middle of the one's last letter to the middle of the other's
		// first, run on to the ink on either side where the window cut it.
		std::optional<std::pair<int, int>> gapBetween( const std::vector<ocr::Character>& chars, std::pair<std::size_t, std::size_t> one, std::pair<std::size_t, std::size_t> other, const InkColumns& ink )
		{
			const int  lo     = std::max( ink.from, centreOf( chars[one.second - 1] ) );
			const int  hi     = std::min( ink.to(), centreOf( chars[other.first] ) );
			const auto runs   = blankRuns( ink, lo, hi, false );
			const auto widest = std::ranges::max_element( runs, {}, []( const std::pair<int, int>& run ) { return run.second - run.first; } );
			if ( widest == runs.end() )
			{
				return std::nullopt;
			}
			auto [from, to] = *widest;
			while ( from > ink.from && !ink( from - 1 ) && from > centreOf( chars[one.first] ) )
			{
				--from;
			}
			while ( to < ink.to() && !ink( to ) && to < centreOf( chars[other.second - 1] ) )
			{
				++to;
			}
			return std::pair{ from, to };
		}

		// Each word's characters spread over its new extent in the proportions they were read in, punctuation on its own
		// ink; spaces fill the gaps.
		void spreadWords( std::vector<ocr::Character>& chars, const Ranges& words, const std::vector<std::pair<float, float>>& extents, const InkColumns& ink, int height )
		{
			for ( std::size_t w = 0; w < words.size(); ++w )
			{
				const auto [first, last] = words[w];
				const float a            = chars[first].from;
				const float b            = chars[last - 1].to;
				const float s            = extents[w].first;
				const float e            = std::max( extents[w].second, s + 1.0F );
				const auto  onto         = [&]( float x ) { return b > a ? s + ( ( x - a ) * ( e - s ) / ( b - a ) ) : s; };

				std::vector<int>              edges{ static_cast<int>( std::lround( s ) ) };
				std::vector<std::string_view> texts;
				for ( std::size_t i = first; i < last; ++i )
				{
					const int to = i + 1 == last ? static_cast<int>( std::lround( e ) ) : static_cast<int>( std::lround( onto( chars[i].to ) ) );
					edges.push_back( std::max( to, edges.back() + 1 ) );
					texts.emplace_back( chars[i].text );
				}
				separatePunctuation( edges, texts, height, ink );
				for ( std::size_t i = first; i < last; ++i )
				{
					chars[i].from = static_cast<float>( edges[i - first] );
					chars[i].to   = static_cast<float>( edges[i - first + 1] );
				}
				if ( w + 1 < words.size() )
				{
					for ( std::size_t i = last; i < words[w + 1].first; ++i )
					{
						chars[i].from = chars[last - 1].to;
						chars[i].to   = std::max( extents[w + 1].first, chars[i].from + 1.0F );
					}
				}
			}
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
			OcrCapture( std::unique_ptr<ScreenReader> screen, std::unique_ptr<TesseractEngine> engine, std::unique_ptr<ocr::PaddleOcr> paddle, const OcrOptions& options, const std::string& models, std::vector<const lang::Language*> languages, std::vector<const lang::Language*> wanted ) :
				screen_( std::move( screen ) ),
				engine_( std::move( engine ) ),
				paddle_( std::move( paddle ) ),
				overlays_( options.overlays ),
				vertical_( options.vertical ),
				unit_( std::max( 16, static_cast<int>( std::lround( 24.0 * options.scale ) ) ) ),
				languages_( std::move( languages ) ),
				wanted_( std::move( wanted ) ),
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

			std::optional<CapturedText> capture( Point point, const WindowInfo& window, CaptureScope scope ) override
			{
				const std::size_t max_chars = scope.characters;
				if ( paddle_ )
				{
					return capturePaddle( point, window, scope );
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
				const Recognition* read = &recognition( crop( grab, line->crop ), tone, line->upscale, { line->core_from, line->core_to } );
				// The crop starts a good way before the pointer, but a long word pointed at near its end may still begin
				// before it: read again from the gap before that word.
				if ( read->words && !vertical_ && line->word_begin < line->crop.x && inFirstWord( *read, point ) )
				{
					Rect wider = line->crop;
					wider.width += wider.x - line->word_begin;
					wider.x = line->word_begin;
					log::debug( "ocr: the word at {},{} starts before the crop: reading from {} on", point.x, point.y, grab.region.x + wider.x );
					read = &recognition( crop( grab, wider ), tone, line->upscale, { line->core_from, line->core_to } );
				}
				return select( *read, point, max_chars );
			}

			std::optional<Rect> bounds( const CapturedText& text, std::size_t length ) override
			{
				const auto* line = static_cast<const OcrLine*>( text.handle.get() );
				if ( line == nullptr || length == 0 )
				{
					return std::nullopt;
				}
				return spanOf( *line, text, length );
			}

			std::vector<Rect> lineBounds( const CapturedText& text, std::size_t length ) override
			{
				const auto* line = static_cast<const OcrLine*>( text.handle.get() );
				if ( line == nullptr || length == 0 )
				{
					return {};
				}
				return spansOf( *line, text, length );
			}

			// Reads words of each language drawn for the purpose, with the same recognisers the captures use.
			void diagnose( std::vector<health::Check>& out ) override
			{
				health::Check            check{ .id = "ocr", .title = "Text in images, games and videos (OCR)" };
				std::vector<std::string> good;
				std::vector<std::string> bad;
				std::vector<std::string> missing;
				for ( const lang::Language* language : wanted_ )
				{
					if ( !std::ranges::contains( languages_, language ) )
					{
						missing.emplace_back( language->name() );
					}
				}
				const auto started = std::chrono::steady_clock::now();
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

				if ( !missing.empty() )
				{
					check.status = health::Severity::Warning;
					check.detail = std::format(
							"{} has no model for {}: download OCR for {} on the Scanning page.",
							description_,
							joinText( missing, ", " ),
							missing.size() == 1 ? "that language" : "those languages"
					);
					check.fix = "open-scanning";
					if ( !bad.empty() )
					{
						check.status = health::Severity::Error;
						check.detail += std::format( " It also misread its test text ({}).", joinText( bad, "; " ) );
					}
					else if ( !good.empty() )
					{
						check.detail += std::format( " It read its test text for the languages it has in {} ms.", elapsed );
					}
				}
				else if ( good.empty() && bad.empty() )
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
			std::optional<CapturedText> capturePaddle( Point point, const WindowInfo& window, CaptureScope scope )
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
				// Each box is read once, when it is first needed.
				const auto read = [&]( std::size_t index ) -> const ocr::TextLine& {
					auto& line = cached->lines[index];
					if ( !line )
					{
						line      = paddle_->recognize( image, cached->boxes[index], vertical_ );
						line->box = inked( image, line->box, line->vertical );
						snapWords( *line, image );
						log::debug( "ocr: {}box {}x{} ({}) «{}»", loose && index == *chosen ? "loose " : "", line->box.width, line->box.height, line->vertical ? "vertical" : "horizontal", lineText( *line ) );
					}
					return *line;
				};
				const ocr::TextLine& line  = read( *chosen );
				const auto           start = pointed( line, local );
				if ( !start )
				{
					return std::nullopt;
				}
				// A sentence the line does not finish goes on on the next lines of its paragraph (wrapped text in a chat,
				// say). They are read when the text from the pointer reaches the end of the line or the sentence is wanted,
				// and the lines it began on only for the sentence: each costs a recognition.
				std::vector<const ocr::TextLine*> preceding;
				std::vector<const ocr::TextLine*> following;
				if ( !loose && ( scope.sentence || line.characters.size() - *start < scope.characters ) )
				{
					following = followingLines( *cached, *chosen, read );
				}
				if ( !loose && scope.sentence )
				{
					preceding = precedingLines( *cached, *chosen, read );
				}
				return selectLine( line, *start, preceding, following, region, scope.characters, loose ? 0.8F : 0.6F );
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

			// Recognition places characters only roughly, where the network noticed them (a little early): in text written
			// with spaces, a word's box would stop short of its last letter. Each word goes onto its ink, from gap to gap
			// (the gaps where the spaces were read), and its letters are spread over it as they were read; spaces left out
			// are put back, and letters read in the wrong script mended. Japanese keeps the network's reading and places.
			static void snapWords( ocr::TextLine& line, const ocr::Image& image )
			{
				auto& chars = line.characters;
				if ( line.vertical || chars.empty() )
				{
					return;
				}
				if ( !wordScript( chars | std::views::transform( &ocr::Character::text ) ) )
				{
					spaceGaps( line, image );
					return;
				}
				const InkColumns ink = inkColumns( image, line.box, chars.size() );
				if ( ink.ink.size() < 2 )
				{
					return;
				}
				const int height = std::max( 1, line.box.inner_height );
				// A space read twice is one.
				const auto doubled = std::ranges::unique( chars, [&]( const ocr::Character& a, const ocr::Character& b ) { return isSpace( a.text ) && isSpace( b.text ); } );
				chars.erase( doubled.begin(), doubled.end() );
				recoverSpaces( chars, ink, height );
				mendHomoglyphs( chars );

				const auto words = wordRanges( chars );
				if ( words.empty() )
				{
					return;
				}
				std::vector<std::pair<float, float>> extents;
				for ( const auto& [first, last] : words )
				{
					extents.emplace_back( chars[first].from, chars[last - 1].to );
				}
				lineEnds( chars, words, extents, ink );
				for ( std::size_t w = 0; w + 1 < words.size(); ++w )
				{
					if ( const auto gap = gapBetween( chars, words[w], words[w + 1], ink ) )
					{
						extents[w].second    = static_cast<float>( gap->first );
						extents[w + 1].first = static_cast<float>( gap->second );
					}
				}
				spreadWords( chars, words, extents, ink, height );
			}

			// The recognised text from the character under the pointer on, if the recognition is sure enough.
			[[nodiscard]] static std::string lineText( const ocr::TextLine& line )
			{
				std::string text;
				for ( const auto& c : line.characters )
				{
					text.append( c.text );
				}
				return text;
			}

			// Whether two lines are in the same script, or one of them has no letters to tell (a line of digits).
			[[nodiscard]] static bool sameScript( const ocr::TextLine& a, const ocr::TextLine& b )
			{
				const int one   = mainScript( a );
				const int other = mainScript( b );
				return one == 0 || other == 0 || one == other;
			}

			// The lines the text of line `chosen` goes on in, in order, until one ends a sentence: at most four, and 400
			// characters. `read` recognises a line.
			template <typename Read>
			[[nodiscard]] static std::vector<const ocr::TextLine*> followingLines( const PaddleRegion& cached, std::size_t chosen, const Read& read )
			{
				std::vector<const ocr::TextLine*> following;
				const ocr::TextLine&              line    = read( chosen );
				std::size_t                       current = chosen;
				std::size_t                       read_in = line.characters.size();
				for ( int more = 0; more < 4 && read_in < 400 && !endsSentence( lineText( following.empty() ? line : *following.back() ) ); ++more )
				{
					const auto next = continuation( cached, current, line.vertical );
					if ( !next || *next == chosen || std::ranges::contains( following, &read( *next ) ) || !sameScript( line, read( *next ) ) )
					{
						break;
					}
					following.push_back( &read( *next ) );
					read_in += following.back()->characters.size();
					current = *next;
				}
				return following;
			}

			// The lines before line `chosen` that its sentence began in, in order: back to one that ends a sentence, at most
			// four.
			template <typename Read>
			[[nodiscard]] static std::vector<const ocr::TextLine*> precedingLines( const PaddleRegion& cached, std::size_t chosen, const Read& read )
			{
				std::vector<const ocr::TextLine*> preceding;
				const ocr::TextLine&              line    = read( chosen );
				std::size_t                       current = chosen;
				for ( int more = 0; more < 4; ++more )
				{
					const auto previous = precedingLine( cached, current, line.vertical );
					if ( !previous || *previous == chosen || std::ranges::contains( preceding, &read( *previous ) ) || !sameScript( line, read( *previous ) ) || endsSentence( lineText( read( *previous ) ) ) )
					{
						break;
					}
					preceding.insert( preceding.begin(), &read( *previous ) );
					current = *previous;
				}
				return preceding;
			}

			// The text box a line goes on in: its other part on the same line when the detector cut it in two; else the next
			// line below (the next column to the left, written vertically) of about its size, starting no later than it and
			// no longer than it by more than a word. None when the line stops short of the others of its column, as the
			// last line of a paragraph or a message does.
			[[nodiscard]] static std::optional<std::size_t> continuation( const PaddleRegion& cached, std::size_t current, bool vertical )
			{
				// Along the reading direction and across it (for columns, right to left becomes increasing).
				struct Span
				{
					int from, to, top, bottom;
				};
				const auto span = [vertical]( const ocr::Box& b ) {
					return vertical ? Span{ .from = b.inner_y, .to = b.inner_y + b.inner_height, .top = -( b.inner_x + b.inner_width ), .bottom = -b.inner_x }
					                : Span{ .from = b.inner_x, .to = b.inner_x + b.inner_width, .top = b.inner_y, .bottom = b.inner_y + b.inner_height };
				};
				const Span a    = span( cached.boxes[current] );
				const int  size = std::max( 1, a.bottom - a.top );
				// Of about the same size: a short line (す。) hugs smaller letters, so the next line may be well under.
				const auto alike = [&]( const Span& b, int low = 7 ) {
					const int other = b.bottom - b.top;
					return other * 10 >= size * low && other * 10 <= size * 16;
				};

				std::optional<std::size_t> best;
				int                        best_gap = std::numeric_limits<int>::max();
				// The rest of the same line, a little further along. The words of a wide font (a pixel font's) stand further
				// apart, each boxed on its own, but on the line's baseline.
				for ( std::size_t i = 0; i < cached.strict; ++i )
				{
					const Span b       = span( cached.boxes[i] );
					const int  overlap = std::min( a.bottom, b.bottom ) - std::max( a.top, b.top );
					const int  gap     = b.from - a.to;
					const int  reach   = !vertical && std::abs( a.bottom - b.bottom ) * 4 <= size ? 4 * size : 2 * size;
					if ( i != current && alike( b ) && overlap * 2 >= size && gap >= -size / 2 && gap <= reach && gap < best_gap )
					{
						best     = i;
						best_gap = gap;
					}
				}
				if ( best )
				{
					return best;
				}

				// A line that wraps reaches the end of its column; one that stops well before it ends its paragraph.
				int column_end = a.to;
				for ( std::size_t i = 0; i < cached.strict; ++i )
				{
					const Span b = span( cached.boxes[i] );
					if ( alike( b ) && b.top > a.top - ( 6 * size ) && b.top < a.bottom + ( 6 * size ) && b.from <= a.from + size && b.to > a.from )
					{
						column_end = std::max( column_end, b.to );
					}
				}
				if ( a.to < column_end - ( 3 * size ) )
				{
					return std::nullopt;
				}
				// The detector's boxes hug the middle of the letters, so lines lie two to four box heights apart (centre to
				// centre: a short line's box is placed by the few letters it has).
				for ( std::size_t i = 0; i < cached.strict; ++i )
				{
					const Span b     = span( cached.boxes[i] );
					const int  pitch = ( b.top + b.bottom - a.top - a.bottom ) / 2;
					// A line of much smaller letters only as a short end of the paragraph (「す。」), not a caption under it.
					const bool sized = alike( b ) || ( alike( b, 4 ) && b.to - b.from <= 4 * size );
					if ( i != current && sized && pitch * 5 >= size * 6 && pitch <= size * 4 && b.from <= a.from + size && b.to > a.from && b.to <= a.to + ( 3 * size ) && pitch < best_gap )
					{
						best     = i;
						best_gap = pitch;
					}
				}
				return best;
			}

			// The line that goes on in this one: the nearest above (to the right, written vertically) whose continuation it is.
			[[nodiscard]] static std::optional<std::size_t> precedingLine( const PaddleRegion& cached, std::size_t current, bool vertical )
			{
				std::optional<std::size_t> best;
				for ( std::size_t i = 0; i < cached.strict; ++i )
				{
					if ( i == current || continuation( cached, i, vertical ) != current )
					{
						continue;
					}
					const auto& b = cached.boxes[i];
					if ( !best || ( vertical ? b.inner_x < cached.boxes[*best].inner_x : b.inner_y > cached.boxes[*best].inner_y ) )
					{
						best = i;
					}
				}
				return best;
			}

			// The character of a line under the pointer; none when the pointer is beside the line.
			[[nodiscard]] static std::optional<std::size_t> pointed( const ocr::TextLine& line, Point local )
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
				// Between two words, the nearer one: the gap is only a few pixels wide.
				if ( isSpace( chars[start].text ) && chars.size() > 1 )
				{
					const bool before = along < ( chars[start].from + chars[start].to ) / 2.0F ? start > 0 : start + 1 >= chars.size();
					start             = before ? start - 1 : start + 1;
				}
				return start;
			}

			// The text from the pointer's character (`start` of `line`) on, and its sentence: the line, with the lines it runs
			// on from and on to.
			[[nodiscard]] static std::optional<CapturedText> selectLine( const ocr::TextLine& line, std::size_t start, std::span<const ocr::TextLine* const> preceding, std::span<const ocr::TextLine* const> following, const Rect& region, std::size_t max_chars, float gate )
			{
				const auto& chars = line.characters;

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

				// The text from there, and the box of each character of the lines for bounds(), which may reach back to the
				// start of the word. Words written with spaces had one where a line broke.
				const auto box_of = [&region]( const ocr::TextLine& of, std::size_t i ) {
					const auto& b    = of.box;
					const int   from = static_cast<int>( std::lround( of.characters[i].from ) );
					const int   to   = std::max( from + 1, static_cast<int>( std::lround( of.characters[i].to ) ) );
					return of.vertical ? Rect{ .x = region.x + b.inner_x, .y = region.y + from, .width = b.inner_width, .height = to - from }
					                   : Rect{ .x = region.x + from, .y = region.y + b.inner_y, .width = to - from, .height = b.inner_height };
				};
				auto        handle = std::make_shared<OcrLine>();
				std::string text;
				std::size_t length = 0;
				std::string sentence;
				std::size_t sentence_offset = 0;
				Rect        character;
				bool        reading = false;
				const auto  take    = [&]( std::string_view piece ) {
					if ( reading && length < max_chars )
					{
						text.append( piece );
						length += std::max<std::size_t>( 1, utf8::length( piece ) );
					}
				};
				const auto add = [&]( const ocr::TextLine& of, std::optional<std::size_t> pointer ) {
					if ( of.characters.empty() )
					{
						return;
					}
					if ( !handle->boxes.empty() )
					{
						const char32_t before = utf8::last( sentence );
						const char32_t after  = utf8::first( of.characters.front().text );
						// A piece the detector cut off on the same row and a gap away from what came before is something
						// else beside it (the columns of a menu, a word beside its reading): it gets a space even in
						// Japanese, which ends the sentence there. A piece that carries straight on from where the last one
						// stopped is the same run of text the detector happened to cut in two (at a bracket, at a quotation
						// mark), and a space there would break the word it cut through. A piece on the next row is the same
						// sentence going on, and gets none either.
						const Rect first   = box_of( of, 0 );
						const Rect end_box = handle->boxes.back();
						const int  along   = of.vertical ? std::min( end_box.x + end_box.width, first.x + first.width ) - std::max( end_box.x, first.x )
						                                 : std::min( end_box.y + end_box.height, first.y + first.height ) - std::max( end_box.y, first.y );
						const int  across  = of.vertical ? std::min( end_box.width, first.width ) : std::min( end_box.height, first.height );
						const int  between = of.vertical ? first.y - ( end_box.y + end_box.height ) : first.x - ( end_box.x + end_box.width );
						// A character of a grid script fills about a square, so a gap of one counts as a break between two
						// things; less than that is the detector's cut, not a space.
						const bool beside = along * 2 > across && between >= across;
						if ( ( beside || !( gridCharacter( before ) && gridCharacter( after ) ) ) && before != U'-' && before != U' ' )
						{
							const Rect& end = handle->boxes.back();
							sentence.append( " " );
							handle->boxes.push_back( of.vertical ? Rect{ .x = end.x, .y = end.y + end.height, .width = end.width, .height = 0 } : Rect{ .x = end.x + end.width, .y = end.y, .width = 0, .height = end.height } );
							take( " " );
						}
						handle->line_starts.push_back( handle->boxes.size() );
					}
					for ( std::size_t i = 0; i < of.characters.size(); ++i )
					{
						const Rect box = box_of( of, i );
						if ( pointer && i == *pointer )
						{
							sentence_offset = sentence.size();
							handle->first   = handle->boxes.size();
							character       = box;
							reading         = true;
						}
						sentence.append( of.characters[i].text );
						addBoxes( handle->boxes, of.characters[i].text, box );
						take( of.characters[i].text );
					}
				};
				for ( const ocr::TextLine* before : preceding )
				{
					add( *before, std::nullopt );
				}
				add( line, start );
				for ( const ocr::TextLine* after : following )
				{
					add( *after, std::nullopt );
				}
				CapturedText captured{ .text = std::move( text ), .offset = 0, .character = character };
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
				// Boxes too small to place a character by, or outside the line. A dash is only a pixel high and a full stop in
				// small text a pixel across, though, which in a script written with spaces are their real boxes, not missing
				// ones.
				const auto unplaceable = [&]( bool thin ) {
					return std::ranges::any_of( found, [&]( const OcrSymbol& symbol ) {
						const bool flat = thin ? symbol.box.width < 2 || symbol.box.height < 2 : symbol.box.width < 2 && symbol.box.height < 2 && !isPunctuation( utf8::first( symbol.text ) );
						return !isSpace( symbol.text ) && ( flat || intersect( symbol.box, grab.region ).empty() );
					} );
				};
				// Japanese is placed on its grid; scripts written with spaces keep Tesseract's letters and get their spaces back.
				const bool words   = !vertical_ && !unplaceable( false ) && wordScript( found | std::views::transform( &OcrSymbol::text ) );
				const bool boxless = !words && unplaceable( true );
				// Words keep each letter where Tesseract saw it, so a phantom shifts nothing; a thin hyphen whose rough box
				// misses its ink is real, though.
				if ( !boxless && !words )
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
				found = words ? placeWords( found, grab, tone, core ) : place( found, grab, tone, vertical_, !boxless, core );
				log::debug( "ocr: {} symbols in {}x{} at {},{} (x{}, {} background{})", found.size(), grab.region.width, grab.region.height, grab.region.x, grab.region.y, upscale, tone.dark ? "dark" : "light", words ? ", words" : "" );

				Recognition& slot = cache_[next_slot_];
				next_slot_        = ( next_slot_ + 1 ) % cache_.size();
				slot              = { .region = grab.region, .hash = key, .symbols = std::move( found ), .words = words };
				return slot;
			}

			// Whether no space comes between the start of a line read as words and the pointer.
			[[nodiscard]] static bool inFirstWord( const Recognition& recognition, Point point )
			{
				const auto& symbols = recognition.symbols;
				const auto  space   = std::ranges::find_if( symbols, []( const OcrSymbol& symbol ) { return isSpace( symbol.text ); } );
				return !symbols.empty() && ( space == symbols.end() || space->box.x > point.x );
			}

			[[nodiscard]] std::pair<int, int> along( const Rect& box ) const
			{
				return vertical_ ? std::pair{ box.y, box.y + box.height } : std::pair{ box.x, box.x + box.width };
			}

			[[nodiscard]] std::pair<int, int> across( const Rect& box ) const
			{
				return vertical_ ? std::pair{ box.x, box.x + box.width } : std::pair{ box.y, box.y + box.height };
			}

			// The line nearest to `cross` across the reading direction; -1 when none is near.
			[[nodiscard]] int nearestLine( const std::vector<OcrSymbol>& symbols, int cross ) const
			{
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
				return line;
			}

			// The box of each symbol of a line: its own, or an even split of the line when they do not all look sound
			// (LSTM symbol boxes are often offset or merged across characters). Letters of words were already put onto
			// their ink, and differ in width. The second is whether they are their own.
			[[nodiscard]] std::pair<std::vector<Rect>, bool> lineBoxes( const std::vector<const OcrSymbol*>& row, int median, bool words ) const
			{
				std::size_t odd = 0;
				for ( std::size_t i = 0; i < row.size() && !words; ++i )
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

				std::vector<Rect> boxes;
				boxes.reserve( row.size() );
				if ( odd == 0 )
				{
					for ( const OcrSymbol* s : row )
					{
						boxes.push_back( s->box );
					}
					return { std::move( boxes ), true };
				}
				const int line_from = along( row.front()->box ).first;
				const int line_to   = along( row.back()->box ).second;
				int       low       = std::numeric_limits<int>::max();
				int       high      = std::numeric_limits<int>::min();
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
				return { std::move( boxes ), false };
			}

			// The character under the pointer (letters of words are set edge to edge), otherwise the one whose centre is
			// nearest; between two words, the nearer one, as the gap is only a few pixels wide.
			[[nodiscard]] std::size_t pointedAt( const std::vector<const OcrSymbol*>& row, const std::vector<Rect>& boxes, int position, bool words ) const
			{
				std::size_t start = 0;
				int         best  = std::numeric_limits<int>::max();
				for ( std::size_t i = 0; i < boxes.size(); ++i )
				{
					const auto [from, to] = along( boxes[i] );
					const int distance    = words && position >= from && position < to ? -1 : std::abs( ( ( from + to ) / 2 ) - position );
					if ( distance < best )
					{
						best  = distance;
						start = i;
					}
				}
				if ( isSpace( row[start]->text ) )
				{
					const auto [from, to] = along( boxes[start] );
					const bool before     = position < ( from + to ) / 2 ? start > 0 : start + 1 >= row.size();
					start                 = before ? start - 1 : start + 1;
				}
				return start;
			}

			// Text of the pointer's line, starting at the character under the pointer.
			[[nodiscard]] std::optional<CapturedText> select( const Recognition& recognition, Point point, std::size_t max_chars ) const
			{
				const auto& symbols  = recognition.symbols;
				const int   position = vertical_ ? point.y : point.x;
				const int   line     = nearestLine( symbols, vertical_ ? point.x : point.y );
				if ( line < 0 )
				{
					log::debug( "ocr: no line near {},{}: {}", point.x, point.y, dumpSymbols( symbols ) );
					return std::nullopt;
				}

				// Lines placed as words keep their spaces, which end what is looked up.
				std::vector<const OcrSymbol*> row;
				for ( const OcrSymbol& symbol : symbols )
				{
					if ( symbol.line == line && ( recognition.words || !isSpace( symbol.text ) ) )
					{
						row.push_back( &symbol );
					}
				}
				std::ranges::stable_sort( row, {}, [&]( const OcrSymbol* s ) { return along( s->box ).first; } );

				std::vector<int> sizes;
				sizes.reserve( row.size() );
				for ( const OcrSymbol* s : row )
				{
					if ( !isSpace( s->text ) )
					{
						const auto [from, to] = along( s->box );
						sizes.push_back( to - from );
					}
				}
				if ( sizes.empty() )
				{
					return std::nullopt;
				}
				std::ranges::nth_element( sizes, sizes.begin() + static_cast<std::ptrdiff_t>( sizes.size() / 2 ) );
				const int median = std::max( 1, sizes[sizes.size() / 2] );

				const int line_from = along( row.front()->box ).first;
				const int line_to   = along( row.back()->box ).second;
				if ( position < line_from - ( median / 2 ) || position > line_to + ( median / 2 ) )
				{
					log::debug( "ocr: {},{} is beside the line: {}", point.x, point.y, dumpSymbols( symbols ) );
					return std::nullopt;
				}

				const auto [boxes, plausible] = lineBoxes( row, median, recognition.words );
				const std::size_t start       = pointedAt( row, boxes, position, recognition.words );

				// The text from there, and the box of each character of the line for bounds(), which may reach back to the
				// start of the word.
				auto        handle = std::make_shared<OcrLine>();
				std::string text;
				std::size_t taken        = 0;
				std::size_t length       = 0;
				bool        ended        = false;
				int         previous_end = along( boxes[start] ).first;
				std::string sentence;
				std::size_t sentence_offset = 0;
				for ( std::size_t i = 0; i < row.size(); ++i )
				{
					if ( i == start )
					{
						sentence_offset = sentence.size();
						handle->first   = handle->boxes.size();
					}
					sentence.append( row[i]->text );
					addBoxes( handle->boxes, row[i]->text, boxes[i] );
					if ( i < start || ended || length >= max_chars )
					{
						continue;
					}
					const auto [from, to] = along( boxes[i] );
					// A wide gap ends the phrase (the next column of a table, a separate label).
					const bool gap = recognition.words ? isSpace( row[i]->text ) && to - from > 2 * median : i > start && from - previous_end > 2 * median;
					if ( gap )
					{
						ended = true;
						continue;
					}
					text.append( row[i]->text );
					length += std::max<std::size_t>( 1, utf8::length( row[i]->text ) );
					++taken;
					previous_end = to;
				}

				if ( log::enabled( log::Level::Debug ) )
				{
					const std::string_view placed = recognition.words ? "word" : ( plausible ? "character" : "evenly split" );
					log::debug( "ocr: line «{}» ({} boxes), reading from #{}: «{}»", sentence, placed, start, text );
				}

				// How sure Tesseract was of what is looked up; marks recovered from the ink and spaces carry no confidence of
				// their own.
				float confidence = 0.0F;
				int   counted    = 0;
				for ( std::size_t i = start; i < std::min( row.size(), start + taken ); ++i )
				{
					if ( !isSpace( row[i]->text ) && ( row[i]->confidence > 0.0F || !isSmallPunctuation( row[i]->text ) ) )
					{
						confidence += row[i]->confidence;
						++counted;
					}
				}

				CapturedText captured{ .text = std::move( text ), .offset = 0, .character = boxes[start] };
				captured.handle          = std::move( handle );
				captured.confidence      = counted > 0 ? confidence / static_cast<float>( counted ) : 0.0F;
				captured.sentence        = std::move( sentence );
				captured.sentence_offset = sentence_offset;
				captured.spaces          = recognition.words;
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
			// Languages the daemon wants to read (enabled, with dictionaries); may be more than languages_ when a model is missing.
			std::vector<const lang::Language*> wanted_;
			std::string                        description_;
			std::array<Recognition, 4>         cache_{};
			std::size_t                        next_slot_ = 0;
		};

	} // namespace

	Result<std::unique_ptr<TextCapture>> createOcrCapture( const OcrOptions& options )
	{
		// PaddleOCR reads stylised, outlined and vertical text far better; Tesseract is the fallback.
		std::unique_ptr<ocr::PaddleOcr> paddle;
		// Why it did not load, kept for the report when Tesseract cannot stand in for it either. Empty unless it was
		// downloaded and still failed, which is a state the user can do something about.
		std::string paddle_error;
		if ( options.engine != config::OcrEngine::Tesseract )
		{
			auto loaded = ocr::PaddleOcr::load( paths::ocrDir() / "paddle", paths::ocrDir() / "runtime", 4, options.languages );
			if ( loaded )
			{
				paddle = std::move( *loaded );
				// Load the networks now instead of on the first scan.
				const ocr::Image blank{ .width = 64, .height = 32, .rgb = std::vector<std::uint8_t>( static_cast<std::size_t>( 64 ) * 32 * 3, 255 ) };
				( void )paddle->detect( blank );
				( void )paddle->recognize( blank, { .x = 0, .y = 0, .width = 64, .height = 32, .score = 1.0F } );
			}
			else if ( options.engine == config::OcrEngine::Paddle )
			{
				return std::unexpected( loaded.error() );
			}
			else
			{
				if ( ocr::loadFailureActionable( loaded.error().message ) )
				{
					paddle_error = loaded.error().message;
				}
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
				// Neither engine can read anything, so the reason PaddleOCR could not is the one worth reporting:
				// Tesseract's own "nothing installed" would hide it behind a download that would not help.
				return paddle_error.empty() ? fail( "no OCR model is installed" ) : fail( "{}", paddle_error );
			}
			auto engine = TesseractEngine::load( *datapath, models, options.vertical );
			if ( !engine )
			{
				return std::unexpected( engine.error() );
			}
			const std::vector<std::uint8_t> blank( static_cast<std::size_t>( 64 ) * 32, 255 );
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
		return std::make_unique<OcrCapture>( std::move( *screen ), std::move( tesseract ), std::move( paddle ), options, models, std::move( languages ), wanted );
	}

} // namespace lexiglance::platform
