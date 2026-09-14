#include <lexiglance/render/PopupRenderer.h>

#include <lexiglance/core/Utf8.h>
#include <lexiglance/dictionary/StructuredContent.h>

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

namespace lexiglance::render
{

	namespace
	{

		// Cairo refuses images beyond 32767 px; long results are cut off (the popup scrolls within this).
		constexpr int max_content_height = 16000;

#ifdef _WIN32
		// Windows' own Japanese fonts after Noto, which may have been installed.
		constexpr std::string_view default_family = "Noto Sans CJK JP, Noto Sans JP, Yu Gothic UI, Meiryo UI, Yu Gothic, Meiryo, MS Gothic, sans-serif";
#else
		// Noto Sans first for Latin and Cyrillic: the CJK fonts put combining marks (the stress in кни́га) over the next
		// letter. Kana and kanji fall through to the Japanese fonts.
		constexpr std::string_view default_family = "Noto Sans, Noto Sans CJK JP, Noto Sans JP, IPAGothic, sans-serif";
#endif

		struct LayoutDeleter
		{
			void operator()( PangoLayout* layout ) const noexcept
			{
				g_object_unref( layout );
			}
		};
		using LayoutPtr = std::unique_ptr<PangoLayout, LayoutDeleter>;

		struct FontDeleter
		{
			void operator()( PangoFontDescription* font ) const noexcept
			{
				pango_font_description_free( font );
			}
		};
		using FontPtr = std::unique_ptr<PangoFontDescription, FontDeleter>;

		struct ContextDeleter
		{
			void operator()( PangoContext* context ) const noexcept
			{
				g_object_unref( context );
			}
		};

		struct FontMapDeleter
		{
			void operator()( PangoFontMap* map ) const noexcept
			{
				g_object_unref( map );
			}
		};

		struct CairoDeleter
		{
			void operator()( cairo_t* cr ) const noexcept
			{
				cairo_destroy( cr );
			}
		};

		struct TextOp
		{
			LayoutPtr layout;
			double    x = 0.0;
			double    y = 0.0;
			Color     color;
			bool      selectable = true;
		};

		struct RectOp
		{
			double x      = 0.0;
			double y      = 0.0;
			double width  = 0.0;
			double height = 0.0;
			double radius = 0.0;
			Color  color;
			bool   round_left  = true;
			bool   round_right = true;
		};

		enum class Icon : std::uint8_t
		{
			Speaker,
			Plus,
			Check
		};

		Icon iconFor( PopupAction action, bool exists )
		{
			if ( action == PopupAction::Audio )
			{
				return Icon::Speaker;
			}
			return exists ? Icon::Check : Icon::Plus;
		}

		struct IconOp
		{
			Icon   icon = Icon::Speaker;
			double x    = 0.0;
			double y    = 0.0;
			double size = 0.0;
			Color  color;
		};

		using Op = std::variant<TextOp, RectOp, IconOp>;

		struct Pill
		{
			std::string label;
			Color       color;
			std::string value;
		};

		void setSource( cairo_t* cr, const Color& color )
		{
			cairo_set_source_rgba( cr, color.r, color.g, color.b, color.a );
		}

		void roundedRect( cairo_t* cr, double x, double y, double w, double h, double r, bool left = true, bool right = true )
		{
			constexpr double pi = std::numbers::pi;
			r                   = std::min( { r, w / 2.0, h / 2.0 } );
			cairo_new_sub_path( cr );
			if ( right )
			{
				cairo_arc( cr, x + w - r, y + r, r, -pi / 2.0, 0.0 );
				cairo_arc( cr, x + w - r, y + h - r, r, 0.0, pi / 2.0 );
			}
			else
			{
				cairo_line_to( cr, x + w, y );
				cairo_line_to( cr, x + w, y + h );
			}
			if ( left )
			{
				cairo_arc( cr, x + r, y + h - r, r, pi / 2.0, pi );
				cairo_arc( cr, x + r, y + r, r, pi, 3.0 * pi / 2.0 );
			}
			else
			{
				cairo_line_to( cr, x, y + h );
				cairo_line_to( cr, x, y );
			}
			cairo_close_path( cr );
		}

		// Vector icons stay crisp at every scale: a speaker for audio, a plus for Anki.
		void drawIcon( cairo_t* cr, const IconOp& icon )
		{
			const double s = icon.size;
			const double x = icon.x;
			const double y = icon.y;
			setSource( cr, icon.color );
			if ( icon.icon == Icon::Speaker )
			{
				cairo_move_to( cr, x + ( s * 0.2 ), y + ( s * 0.39 ) );
				cairo_line_to( cr, x + ( s * 0.34 ), y + ( s * 0.39 ) );
				cairo_line_to( cr, x + ( s * 0.52 ), y + ( s * 0.22 ) );
				cairo_line_to( cr, x + ( s * 0.52 ), y + ( s * 0.78 ) );
				cairo_line_to( cr, x + ( s * 0.34 ), y + ( s * 0.61 ) );
				cairo_line_to( cr, x + ( s * 0.2 ), y + ( s * 0.61 ) );
				cairo_close_path( cr );
				cairo_fill( cr );
				cairo_set_line_width( cr, std::max( 1.0, s * 0.075 ) );
				cairo_set_line_cap( cr, CAIRO_LINE_CAP_ROUND );
				cairo_arc( cr, x + ( s * 0.52 ), y + ( s * 0.5 ), s * 0.17, -0.85, 0.85 );
				cairo_stroke( cr );
				cairo_arc( cr, x + ( s * 0.52 ), y + ( s * 0.5 ), s * 0.3, -0.85, 0.85 );
				cairo_stroke( cr );
			}
			else if ( icon.icon == Icon::Check )
			{
				cairo_set_line_width( cr, std::max( 1.5, s * 0.11 ) );
				cairo_set_line_cap( cr, CAIRO_LINE_CAP_ROUND );
				cairo_set_line_join( cr, CAIRO_LINE_JOIN_ROUND );
				cairo_move_to( cr, x + ( s * 0.27 ), y + ( s * 0.52 ) );
				cairo_line_to( cr, x + ( s * 0.44 ), y + ( s * 0.68 ) );
				cairo_line_to( cr, x + ( s * 0.74 ), y + ( s * 0.34 ) );
				cairo_stroke( cr );
			}
			else
			{
				const double t = std::max( 1.0, std::round( s * 0.1 ) );
				cairo_rectangle( cr, x + ( s * 0.26 ), y + ( ( s - t ) / 2.0 ), s * 0.48, t );
				cairo_rectangle( cr, x + ( ( s - t ) / 2.0 ), y + ( s * 0.26 ), t, s * 0.48 );
				cairo_fill( cr );
			}
		}

		bool isSmallKana( char32_t c ) noexcept
		{
			constexpr std::u32string_view small = U"ゃゅょぁぃぅぇぉゎャュョァィゥェォヮ";
			return small.contains( c );
		}

		// Reading with a downstep mark after the accented mora, e.g. たべꜜる for position 2.
		std::string pitchText( std::string_view reading, int position )
		{
			std::vector<std::string> morae;
			for ( const char32_t c : utf8::codepoints( reading ) )
			{
				if ( !morae.empty() && isSmallKana( c ) )
				{
					utf8::append( morae.back(), c );
				}
				else
				{
					morae.emplace_back();
					utf8::append( morae.back(), c );
				}
			}

			std::string out;
			for ( std::size_t i = 0; i < morae.size(); ++i )
			{
				out.append( morae[i] );
				if ( position > 0 && static_cast<std::size_t>( position ) == i + 1 )
				{
					out.append( "ꜜ" );
				}
			}
			return out;
		}

		// "JMdict [2026-09-12]" -> "JMdict": revision suffixes only add noise to a popup.
		std::string shortName( std::string_view title )
		{
			if ( const auto bracket = title.rfind( " [" ); bracket != std::string_view::npos && bracket > 0 && title.ends_with( ']' ) )
			{
				title = title.substr( 0, bracket );
			}
			return std::string( title );
		}

		bool isNumber( std::string_view text ) noexcept
		{
			return !text.empty() && std::ranges::all_of( text, []( char c ) { return c >= '0' && c <= '9'; } );
		}

		std::vector<std::string_view> splitTags( std::string_view tags )
		{
			std::vector<std::string_view> out;
			while ( !tags.empty() )
			{
				const auto space = tags.find( ' ' );
				if ( space != 0 )
				{
					out.push_back( tags.substr( 0, space ) );
				}
				if ( space == std::string_view::npos )
				{
					break;
				}
				tags.remove_prefix( space + 1 );
			}
			return out;
		}

		// A tag in plain words: its explanation without the asides in parentheses ("noun (common) (futsuumeishi)" gives
		// "Noun"), or the tag itself when there is no short explanation.
		std::string friendlyLabel( std::string_view name, std::string_view notes )
		{
			std::string text;
			int         depth = 0;
			for ( const char c : notes )
			{
				if ( c == '(' )
				{
					++depth;
				}
				else if ( c == ')' )
				{
					depth = std::max( 0, depth - 1 );
				}
				else if ( depth == 0 && ( c != ' ' || ( !text.empty() && text.back() != ' ' ) ) )
				{
					text.push_back( c );
				}
			}
			while ( !text.empty() && ( text.back() == ' ' || text.back() == ',' || text.back() == ';' ) )
			{
				text.pop_back();
			}
			if ( text.empty() || utf8::length( text ) > 24 )
			{
				text = std::string( name );
			}
			if ( !text.empty() && text.front() >= 'a' && text.front() <= 'z' )
			{
				text.front() = static_cast<char>( text.front() - 'a' + 'A' );
			}
			return text;
		}

		std::string escaped( std::string_view text )
		{
			std::string out;
			dict::escapeMarkup( out, text, dict::Markup::Pango );
			return out;
		}

		// "causative « passive « past" as a path from the dictionary form: "causative -> passive -> past".
		std::string inflectionPath( std::string chain )
		{
			constexpr std::string_view from = " « ";
			constexpr std::string_view to   = " -> ";
			for ( auto at = chain.find( from ); at != std::string::npos; at = chain.find( from, at + to.size() ) )
			{
				chain.replace( at, from.size(), to );
			}
			return chain;
		}

		bool structured( const dict::Dictionary& dictionary, const dict::format::TermRecord& term )
		{
			return std::ranges::any_of( dictionary.glossary( term ), []( const dict::format::GlossRecord& gloss ) { return gloss.kind == dict::format::GlossKind::StructuredContent; } );
		}

	} // namespace

	int PopupStyle::px( double css ) const noexcept
	{
		return static_cast<int>( std::lround( css * scale ) );
	}

	PopupImage::~PopupImage()
	{
		cairo_surface_destroy( surface_ );
	}

	std::optional<std::size_t> PopupImage::glyphAt( int x, int y ) const noexcept
	{
		std::optional<std::size_t> best;
		double                     best_distance = std::numeric_limits<double>::max();
		for ( std::size_t i = 0; i < glyphs_.size(); ++i )
		{
			const Glyph& g  = glyphs_[i];
			const double dx = std::max( { 0.0, static_cast<double>( g.x ) - x, x - static_cast<double>( g.x + g.width ) } );
			const double dy = std::max( { 0.0, static_cast<double>( g.y ) - y, y - static_cast<double>( g.y + g.height ) } );
			// Vertical distance weighs more, so the pointer's own line wins.
			const double distance = ( dy * 4.0 ) + dx;
			if ( distance < best_distance )
			{
				best_distance = distance;
				best          = i;
			}
		}
		return best;
	}

	std::string PopupImage::selectedText( std::size_t first, std::size_t last ) const
	{
		if ( glyphs_.empty() )
		{
			return {};
		}
		if ( first > last )
		{
			std::swap( first, last );
		}
		last = std::min( last, glyphs_.size() - 1 );
		std::string out;
		for ( std::size_t i = first; i <= last; ++i )
		{
			const Glyph& g = glyphs_[i];
			if ( i > first && !out.empty() )
			{
				const Glyph& p        = glyphs_[i - 1];
				const bool   new_line = g.y >= p.y + ( p.height * 0.6F ) || g.y + ( g.height * 0.6F ) <= p.y;
				if ( new_line && out.back() != '\n' )
				{
					out.push_back( '\n' );
				}
				else if ( !new_line && g.x - ( p.x + p.width ) > p.height * 0.25F && out.back() != ' ' )
				{
					out.push_back( ' ' );
				}
			}
			out.append( text_, g.begin, g.end - g.begin );
		}
		const auto first_kept = out.find_first_not_of( " \n" );
		const auto last_kept  = out.find_last_not_of( " \n" );
		return first_kept == std::string::npos ? std::string() : out.substr( first_kept, last_kept - first_kept + 1 );
	}

	struct PopupRenderer::Impl
	{
		Impl() :
			font_map( pango_cairo_font_map_new() ),
			context( pango_font_map_create_context( font_map.get() ) )
		{
			cairo_font_options_t* options = cairo_font_options_create();
			cairo_font_options_set_antialias( options, CAIRO_ANTIALIAS_GRAY );
			cairo_font_options_set_hint_style( options, CAIRO_HINT_STYLE_SLIGHT );
			cairo_font_options_set_hint_metrics( options, CAIRO_HINT_METRICS_OFF );
			pango_cairo_context_set_font_options( context.get(), options );
			cairo_font_options_destroy( options );
			applyStyle();
		}

		[[nodiscard]] FontPtr font( double css_size, PangoWeight weight = PANGO_WEIGHT_NORMAL, PangoStyle slant = PANGO_STYLE_NORMAL ) const
		{
			FontPtr           description( pango_font_description_new() );
			const std::string family = fontFamilies( style.font_family );
			pango_font_description_set_family( description.get(), family.c_str() );
			pango_font_description_set_absolute_size( description.get(), css_size * style.scale * PANGO_SCALE );
			pango_font_description_set_weight( description.get(), weight );
			pango_font_description_set_style( description.get(), slant );
			return description;
		}

		void applyStyle()
		{
			const double f        = style.font_size;
			const bool   friendly = style.design == Design::Friendly;
			const bool   compact  = style.design == Design::Compact;
			// Furigana big enough to read at a glance is what the friendly design is about.
			double headword_factor = 1.75;
			double ruby_factor     = 0.78;
			if ( friendly )
			{
				headword_factor = 2.05;
				ruby_factor     = 1.1;
			}
			else if ( compact )
			{
				headword_factor = 1.35;
				ruby_factor     = 0.7;
			}
			body     = font( f );
			bold     = font( f, PANGO_WEIGHT_BOLD );
			headword = font( style.headword_size > 0.0 ? style.headword_size : f * headword_factor, friendly ? PANGO_WEIGHT_MEDIUM : PANGO_WEIGHT_NORMAL );
			ruby     = font( style.furigana_size > 0.0 ? style.furigana_size : f * ruby_factor );
			small    = font( f * 0.86 );
			italic   = font( f * 0.86, PANGO_WEIGHT_NORMAL, PANGO_STYLE_ITALIC );
			pill     = font( f * 0.74, PANGO_WEIGHT_BOLD );
			kanji    = font( f * 4.2 );
			caption  = font( f * 0.8, PANGO_WEIGHT_BOLD );
			chip     = font( f * 0.8, PANGO_WEIGHT_MEDIUM );
			number   = font( f * 0.92, PANGO_WEIGHT_BOLD );
		}

		LayoutPtr layout( const PangoFontDescription* description, std::string_view text, int width = -1, bool markup = false ) const
		{
			LayoutPtr result( pango_layout_new( context.get() ) );
			pango_layout_set_font_description( result.get(), description );
			if ( width > 0 )
			{
				pango_layout_set_width( result.get(), width * PANGO_SCALE );
				pango_layout_set_wrap( result.get(), PANGO_WRAP_WORD_CHAR );
			}

			const std::string owned( text );
			if ( markup && pango_parse_markup( owned.c_str(), -1, 0, nullptr, nullptr, nullptr, nullptr ) != 0 )
			{
				pango_layout_set_markup( result.get(), owned.c_str(), -1 );
			}
			else
			{
				pango_layout_set_text( result.get(), owned.c_str(), -1 );
			}
			return result;
		}

		static std::pair<int, int> size( PangoLayout* layout )
		{
			int w = 0;
			int h = 0;
			pango_layout_get_pixel_size( layout, &w, &h );
			return { w, h };
		}

		PopupStyle                                    style;
		std::unique_ptr<PangoFontMap, FontMapDeleter> font_map;
		std::unique_ptr<PangoContext, ContextDeleter> context;
		FontPtr                                       body;
		FontPtr                                       bold;
		FontPtr                                       headword;
		FontPtr                                       ruby;
		FontPtr                                       small;
		FontPtr                                       italic;
		FontPtr                                       pill;
		FontPtr                                       kanji;
		FontPtr                                       caption;
		FontPtr                                       chip;
		FontPtr                                       number;
	};

	namespace
	{

		// Lays the popup out as a list of draw operations, then paints them into an image of the final height.
		class Builder
		{
		public:
			Builder( const PopupRenderer::Impl& impl, const lookup::LookupResult& result, std::span<const NoteState> notes, int limit ) :
				impl_( &impl ),
				notes_( notes ),
				style_( &impl.style ),
				result_( &result ),
				set_( result.dictionaries.get() ),
				padding_( style_->px( style_->padding ) ),
				width_( style_->px( style_->width ) ),
				content_width_( width_ - ( 2 * padding_ ) ),
				y_( padding_ ),
				limit_( limit > 0 ? limit : max_content_height )
			{
			}

			std::shared_ptr<const PopupImage> build()
			{
				// A selection made with the wheel that no entry spans is shown as it is, so it can still be read and copied.
				const bool selection = result_->selected > result_->matched_length;
				if ( selection )
				{
					selected();
				}
				// Regions include the separator above them, so every pixel between entries belongs to one.
				for ( std::size_t i = 0; i < result_->terms.size() && y_ < limit_; ++i )
				{
					const int top = i == 0 && !selection ? 0 : static_cast<int>( y_ );
					if ( i > 0 || selection )
					{
						separator();
					}
					term( result_->terms[i], i );
					regions_.push_back( { .top = top, .bottom = static_cast<int>( std::ceil( y_ ) ), .text = std::string( result_->terms[i].expression ) } );
				}
				if ( result_->terms.empty() && style_->show_kanji )
				{
					for ( const auto& entry : result_->kanji )
					{
						const int top = static_cast<int>( y_ );
						kanji( entry );
						const auto& dictionary = *( *set_ )[entry.dictionary].dictionary;
						regions_.push_back( { .top = top, .bottom = static_cast<int>( std::ceil( y_ ) ), .text = std::string( dictionary.string( entry.record->character ) ) } );
					}
				}
				return paint();
			}

		private:
			[[nodiscard]] const Theme& theme() const noexcept
			{
				return style_->theme;
			}

			void text( LayoutPtr layout, double x, double y, const Color& color, bool selectable = true )
			{
				ops_.emplace_back( TextOp{ .layout = std::move( layout ), .x = x, .y = y, .color = color, .selectable = selectable } );
			}

			void selected()
			{
				std::u32string characters;
				for ( const char32_t c : utf8::codepoints( result_->text ) )
				{
					if ( characters.size() == result_->selected )
					{
						break;
					}
					characters.push_back( c );
				}
				const std::string shown       = utf8::fromUtf32( characters );
				const int         top         = 0;
				auto              layout      = impl_->layout( impl_->headword.get(), shown, content_width_ );
				const int         height      = PopupRenderer::Impl::size( layout.get() ).second;
				auto              note        = impl_->layout( impl_->italic.get(), "No entry covers the whole selection", content_width_ );
				const int         note_height = PopupRenderer::Impl::size( note.get() ).second;
				text( std::move( layout ), padding_, y_, theme().text );
				y_ += height;
				text( std::move( note ), padding_, y_, theme().muted, false );
				y_ += note_height;
				regions_.push_back( { .top = top, .bottom = static_cast<int>( std::ceil( y_ ) ), .text = shown } );
			}

			[[nodiscard]] bool classic() const noexcept
			{
				return style_->design == Design::Classic;
			}

			[[nodiscard]] bool compact() const noexcept
			{
				return style_->design == Design::Compact;
			}

			void separator()
			{
				double space = 8;
				if ( compact() )
				{
					space = 5;
				}
				else if ( classic() )
				{
					space = 7;
				}
				y_ += style_->px( space );
				// Friendly entries start with a band of their own, which separates them well enough.
				if ( style_->design != Design::Friendly )
				{
					ops_.emplace_back(
							RectOp{ .x      = static_cast<double>( padding_ ),
					                .y      = y_,
					                .width  = static_cast<double>( content_width_ ),
					                .height = std::max( 1.0, style_->scale ),
					                .color  = theme().separator }
					);
				}
				y_ += style_->px( space + 1 );
			}

			// Soft, rounded labels (friendly design): the tag's colour faded into the background.
			double chips( const std::vector<Pill>& items, double x0 )
			{
				if ( items.empty() )
				{
					return 0.0;
				}
				const bool   dark  = contrast( theme().background, Color{ .r = 0.0, .g = 0.0, .b = 0.0 } ) < 5.0;
				const double pad_x = style_->px( 7 );
				const double pad_y = std::max( 1.0, style_->scale * 2.0 );
				const double gap   = style_->px( 5 );
				const double limit = padding_ + content_width_;
				double       x     = x0;
				double       y     = y_;
				double       row   = 0.0;
				for ( const Pill& item : items )
				{
					auto label          = impl_->layout( impl_->chip.get(), item.label );
					const auto [lw, lh] = PopupRenderer::Impl::size( label.get() );
					const double w      = lw + ( 2 * pad_x );
					const double h      = lh + ( 2 * pad_y );
					if ( x > x0 && x + w > limit )
					{
						x = x0;
						y += row + gap;
						row = 0.0;
					}
					const Color background = mix( item.color, theme().background, dark ? 0.7 : 0.84 );
					const Color foreground = mix( item.color, theme().text, dark ? 0.1 : 0.5 );
					ops_.emplace_back( RectOp{ .x = x, .y = y, .width = w, .height = h, .radius = h / 2.0, .color = background } );
					text( std::move( label ), x + pad_x, y + pad_y, contrast( foreground, background ) >= 3.0 ? foreground : theme().text, false );
					x += w + gap;
					row = std::max( row, h );
				}
				return ( y - y_ ) + row;
			}

			// A quiet line: a muted label, then the text.
			void quietLine( std::string_view label, std::string_view content )
			{
				if ( content.empty() )
				{
					return;
				}
				const std::string markup = std::format( "<span foreground=\"{}\">{}</span>  {}", theme().muted.hex(), escaped( label ), escaped( content ) );
				auto              layout = impl_->layout( impl_->small.get(), markup, content_width_, true );
				const auto        height = PopupRenderer::Impl::size( layout.get() ).second;
				text( std::move( layout ), padding_, y_, theme().text );
				y_ += height + style_->px( 2 );
			}

			// A dictionary's name above its senses, with a hairline to the edge.
			void caption( const std::string& name )
			{
				auto layout       = impl_->layout( impl_->caption.get(), name );
				const auto [w, h] = PopupRenderer::Impl::size( layout.get() );
				text( std::move( layout ), padding_, y_, theme().accent, false );
				const double gap = style_->px( 8 );
				if ( content_width_ > w + gap )
				{
					ops_.emplace_back( RectOp{ .x = padding_ + w + gap, .y = std::round( y_ + ( h / 2.0 ) ), .width = content_width_ - w - gap, .height = std::max( 1.0, style_->scale ), .color = theme().separator } );
				}
				y_ += h + style_->px( compact() ? 1 : 3 );
			}

			// Flows pills left to right, wrapping at the content edge. Returns the height used.
			double pills( const std::vector<Pill>& items, double x0 )
			{
				if ( items.empty() )
				{
					return 0.0;
				}
				const double pad_x  = style_->px( 5 );
				const double pad_y  = std::max( 1.0, style_->scale * 1.5 );
				const double gap    = style_->px( 4 );
				const double radius = style_->px( 3 );
				const double limit  = padding_ + content_width_;

				double x          = x0;
				double y          = y_;
				double row_height = 0.0;
				for ( const Pill& item : items )
				{
					auto label          = impl_->layout( impl_->pill.get(), item.label );
					const auto [lw, lh] = PopupRenderer::Impl::size( label.get() );
					LayoutPtr value;
					int       vw = 0;
					if ( !item.value.empty() )
					{
						value = impl_->layout( impl_->pill.get(), item.value );
						vw    = PopupRenderer::Impl::size( value.get() ).first;
					}
					const double label_w = lw + ( 2 * pad_x );
					const double total_w = label_w + ( value ? vw + ( 2 * pad_x ) : 0.0 );
					const double h       = lh + ( 2 * pad_y );

					if ( x > x0 && x + total_w > limit )
					{
						x = x0;
						y += row_height + gap;
						row_height = 0.0;
					}

					if ( value )
					{
						ops_.emplace_back( RectOp{ .x = x, .y = y, .width = total_w, .height = h, .radius = radius, .color = theme().frequency_value } );
						ops_.emplace_back( RectOp{ .x = x, .y = y, .width = label_w, .height = h, .radius = radius, .color = item.color, .round_right = false } );
						text( std::move( value ), x + label_w + pad_x, y + pad_y, theme().pill_text );
					}
					else
					{
						ops_.emplace_back( RectOp{ .x = x, .y = y, .width = total_w, .height = h, .radius = radius, .color = item.color } );
					}
					text( std::move( label ), x + pad_x, y + pad_y, theme().pill_text );

					x += total_w + gap;
					row_height = std::max( row_height, h );
				}
				return ( y - y_ ) + row_height;
			}

			// Audio and Anki buttons at the right end of the headword line.
			void actions( std::size_t entry, double line_top, int line_height )
			{
				std::vector<PopupAction> wanted;
				if ( style_->anki_button )
				{
					wanted.push_back( PopupAction::Anki );
				}
				if ( style_->audio_button )
				{
					wanted.push_back( PopupAction::Audio );
				}
				const int    size = style_->px( style_->font_size * 1.55 );
				const int    gap  = style_->px( 4 );
				int          x    = padding_ + content_width_ - size;
				const double y    = std::round( line_top + ( ( line_height - size ) / 2.0 ) );
				for ( const PopupAction action : wanted )
				{
					const bool exists = action == PopupAction::Anki && entry < notes_.size() && notes_[entry] == NoteState::Exists;
					ops_.emplace_back( RectOp{ .x = static_cast<double>( x ), .y = y, .width = static_cast<double>( size ), .height = static_cast<double>( size ), .radius = static_cast<double>( style_->px( 4 ) ), .color = exists ? theme().tag_frequency : theme().button } );
					ops_.emplace_back( IconOp{ .icon = iconFor( action, exists ), .x = static_cast<double>( x ), .y = y, .size = static_cast<double>( size ), .color = exists ? theme().pill_text : theme().button_icon } );
					buttons_.push_back( { .action = action, .entry = entry, .x = x, .y = static_cast<int>( y ), .width = size, .height = size } );
					x -= size + gap;
				}
			}

			void headword( const lookup::TermEntry& entry, std::size_t index )
			{
				// As the entry's language writes it; without furigana, the reading goes next to the headword.
				auto       segments    = result_->headword( entry );
				const bool has_reading = std::ranges::any_of( segments, []( const auto& s ) { return !s.reading.empty(); } );
				if ( !style_->show_furigana )
				{
					for ( auto& segment : segments )
					{
						segment.reading.clear();
					}
				}
				const bool any_ruby = has_reading && style_->show_furigana;

				int ruby_height = 0;
				if ( any_ruby )
				{
					ruby_height = PopupRenderer::Impl::size( impl_->layout( impl_->ruby.get(), "あ" ).get() ).second;
				}

				double x           = padding_;
				int    text_height = 0;
				// Furigana in the accent colour stands apart from the headword; Yomitan's look keeps it muted.
				const Color ruby_color = classic() ? theme().muted : theme().reading;
				for ( const auto& segment : segments )
				{
					auto base           = impl_->layout( impl_->headword.get(), segment.text );
					const auto [bw, bh] = PopupRenderer::Impl::size( base.get() );
					int       rw        = 0;
					LayoutPtr reading;
					if ( !segment.reading.empty() )
					{
						reading = impl_->layout( impl_->ruby.get(), segment.reading );
						rw      = PopupRenderer::Impl::size( reading.get() ).first;
					}
					const int w = std::max( bw, rw );
					if ( reading )
					{
						text( std::move( reading ), x + ( ( w - rw ) / 2.0 ), y_, ruby_color, false );
					}
					text( std::move( base ), x + ( ( w - bw ) / 2.0 ), y_ + ruby_height, theme().text );
					x += w;
					text_height = std::max( text_height, bh );
				}

				if ( !style_->show_furigana && style_->show_reading && has_reading )
				{
					auto reading        = impl_->layout( impl_->body.get(), classic() || compact() ? std::format( "【{}】", entry.reading ) : std::string( entry.reading ) );
					const auto [rw, rh] = PopupRenderer::Impl::size( reading.get() );
					text( std::move( reading ), x + style_->px( classic() ? 4 : 8 ), y_ + ruby_height + text_height - rh - style_->px( 2 ), classic() ? theme().muted : theme().reading );
				}
				actions( index, y_ + ruby_height, text_height );
				y_ += ruby_height + text_height + style_->px( classic() ? 2 : 4 );
			}

			void term( const lookup::TermEntry& entry, std::size_t index )
			{
				if ( classic() )
				{
					classicTerm( entry, index );
				}
				else
				{
					plainTerm( entry, index );
				}
			}

			// Word class and the like of an entry in plain words (from its best dictionary), at most five.
			[[nodiscard]] std::vector<Pill> labelsOf( const lookup::TermEntry& entry ) const
			{
				std::vector<Pill>        out;
				std::vector<std::string> seen;
				if ( entry.definitions.empty() )
				{
					return out;
				}
				const auto first = entry.definitions.front().dictionary;
				const auto add   = [&]( const dict::Dictionary& dictionary, std::string_view name ) {
                    if ( isNumber( name ) || out.size() >= 5 )
                    {
                        return;
                    }
                    const auto* tag   = dictionary.findTag( name );
                    std::string label = friendlyLabel( name, tag != nullptr ? dictionary.string( tag->notes ) : std::string_view() );
                    if ( std::ranges::contains( seen, label ) )
                    {
                        return;
                    }
                    seen.push_back( label );
                    out.push_back( { .label = std::move( label ), .color = theme().tag( tag != nullptr ? dictionary.string( tag->category ) : "" ) } );
				};
				for ( const auto& definition : entry.definitions )
				{
					if ( definition.dictionary != first )
					{
						break;
					}
					const auto& dictionary = *( *set_ )[definition.dictionary].dictionary;
					const auto& term       = dictionary.terms()[definition.term];
					for ( const auto name : splitTags( dictionary.string( term.term_tags ) ) )
					{
						add( dictionary, name );
					}
					for ( const auto name : splitTags( dictionary.string( term.definition_tags ) ) )
					{
						add( dictionary, name );
					}
				}
				return out;
			}

			// The friendly and compact designs: plain words instead of dictionary codes, numbered senses under the name of
			// each dictionary, frequency and pitch accent as quiet lines at the end.
			void plainTerm( const lookup::TermEntry& entry, std::size_t index )
			{
				const std::size_t band = ops_.size();
				const double      top  = y_;
				headword( entry, index );

				// Labels shown with the headword are not repeated before each sense.
				std::vector<std::string> shown;
				if ( style_->show_tags )
				{
					const auto labels = labelsOf( entry );
					for ( const auto& label : labels )
					{
						shown.push_back( label.label );
					}
					if ( compact() )
					{
						std::string line;
						for ( const auto& label : labels )
						{
							line.append( line.empty() ? "" : " · " ).append( label.label );
						}
						if ( !line.empty() )
						{
							auto       layout = impl_->layout( impl_->small.get(), line, content_width_ );
							const auto height = PopupRenderer::Impl::size( layout.get() ).second;
							text( std::move( layout ), padding_, y_, theme().muted );
							y_ += height + style_->px( 1 );
						}
					}
					else if ( !labels.empty() )
					{
						y_ += chips( labels, padding_ ) + style_->px( 6 );
					}
				}

				if ( style_->show_inflection && ( entry.inflection_count > 0 || !entry.form_of.empty() ) )
				{
					quietLine( "Form", inflectionPath( result_->inflectionText( entry ) ) );
				}

				// A soft band behind the word, its labels and how it was conjugated: where each entry starts is plain at a
				// glance. Painted first, under all of it.
				if ( style_->design == Design::Friendly )
				{
					const double bleed = style_->px( 6 );
					ops_.insert(
							ops_.begin() + static_cast<std::ptrdiff_t>( band ),
							RectOp{ .x      = padding_ - bleed,
					                .y      = top - bleed,
					                .width  = content_width_ + ( 2 * bleed ),
					                .height = y_ - top + ( 2 * bleed ),
					                .radius = static_cast<double>( style_->px( 8 ) ),
					                .color  = mix( theme().accent, theme().background, 0.9 ) }
					);
					y_ += bleed;
				}

				plainDefinitions( entry, shown );

				if ( style_->show_frequencies && !entry.frequencies.empty() )
				{
					std::string line;
					for ( std::size_t i = 0; i < entry.frequencies.size(); )
					{
						const auto  dictionary = entry.frequencies[i].dictionary;
						std::string values;
						for ( ; i < entry.frequencies.size() && entry.frequencies[i].dictionary == dictionary; ++i )
						{
							const auto& f = entry.frequencies[i];
							values.append( values.empty() ? "" : ", " ).append( f.display.empty() ? std::to_string( f.value ) : std::string( f.display ) );
						}
						line.append( line.empty() ? "" : " · " ).append( std::format( "{} {}", shortName( ( *set_ )[dictionary].name ), values ) );
					}
					quietLine( "Frequency", line );
				}
				if ( style_->show_pitch && !entry.pitches.empty() )
				{
					std::string line;
					for ( const auto& p : entry.pitches )
					{
						const std::string notation = p.position >= 0 ? std::format( "{} [{}]", pitchText( entry.reading, p.position ), p.position ) : std::format( "{} [{}]", entry.reading, p.pattern );
						if ( !line.contains( notation ) )
						{
							line.append( line.empty() ? "" : " · " ).append( notation );
						}
					}
					quietLine( "Pitch accent", line );
				}
			}

			void plainDefinitions( const lookup::TermEntry& entry, const std::vector<std::string>& shown )
			{
				// The sense numbers' column, so definitions line up.
				const int   column = PopupRenderer::Impl::size( impl_->layout( impl_->number.get(), "88" ).get() ).first + style_->px( 6 );
				std::size_t i      = 0;
				while ( i < entry.definitions.size() )
				{
					const auto  dictionary = entry.definitions[i].dictionary;
					std::size_t end        = i;
					while ( end < entry.definitions.size() && entry.definitions[end].dictionary == dictionary )
					{
						++end;
					}
					if ( style_->show_dictionary )
					{
						int gap = compact() ? 3 : 6;
						if ( i == 0 )
						{
							gap = 2;
						}
						y_ += style_->px( gap );
						caption( shortName( ( *set_ )[dictionary].name ) );
					}
					const std::size_t count   = end - i;
					const std::size_t visible = style_->max_senses > 0 ? std::min( count, static_cast<std::size_t>( style_->max_senses ) ) : count;
					for ( std::size_t k = 0; k < visible; ++k )
					{
						sense( entry.definitions[i + k], k + 1, count > 1 && !compact(), column, shown );
					}
					if ( visible < count )
					{
						auto       more   = impl_->layout( impl_->small.get(), std::format( "+ {} more", count - visible ) );
						const auto height = PopupRenderer::Impl::size( more.get() ).second;
						text( std::move( more ), padding_ + ( compact() ? 0 : column ), y_, theme().muted, false );
						y_ += height + style_->px( 2 );
					}
					i = end;
				}
			}

			void sense( const lookup::TermDefinition& definition, std::size_t number, bool numbered, int column, const std::vector<std::string>& shown )
			{
				const auto& loaded     = ( *set_ )[definition.dictionary];
				const auto& dictionary = *loaded.dictionary;
				const auto& term       = dictionary.terms()[definition.term];
				std::string markup     = glossaryMarkup( dictionary, loaded, term );
				if ( markup.empty() )
				{
					return;
				}
				// The sense's own word class, as a quiet prefix.
				if ( style_->show_tags )
				{
					std::string labels;
					for ( const auto name : splitTags( dictionary.string( term.definition_tags ) ) )
					{
						if ( isNumber( name ) )
						{
							continue;
						}
						const auto* tag   = dictionary.findTag( name );
						const auto  label = friendlyLabel( name, tag != nullptr ? dictionary.string( tag->notes ) : std::string_view() );
						if ( !std::ranges::contains( shown, label ) )
						{
							labels.append( labels.empty() ? "" : " · " ).append( label );
						}
					}
					if ( !labels.empty() )
					{
						markup = std::format( R"(<span foreground="{}" size="smaller">{}</span>{})", theme().muted.hex(), escaped( labels ), structured( dictionary, term ) ? "\n" : "  " ) + markup;
					}
				}
				// Structured definitions (Jitendex and the like) number and lay out their own senses.
				numbered = numbered && !structured( dictionary, term );
				int x    = padding_;
				if ( numbered )
				{
					text( impl_->layout( impl_->number.get(), std::to_string( number ) ), x, y_, theme().accent, false );
					x += column;
				}
				auto       body   = impl_->layout( impl_->body.get(), markup, content_width_ - ( x - padding_ ), true );
				const auto height = PopupRenderer::Impl::size( body.get() ).second;
				text( std::move( body ), x, y_, theme().text );
				y_ += height + style_->px( compact() ? 1 : 4 );
			}

			void classicTerm( const lookup::TermEntry& entry, std::size_t index )
			{
				headword( entry, index );

				if ( style_->show_inflection && ( entry.inflection_count > 0 || !entry.form_of.empty() ) )
				{
					auto       chain  = impl_->layout( impl_->italic.get(), "« " + result_->inflectionText( entry ), content_width_ );
					const auto height = PopupRenderer::Impl::size( chain.get() ).second;
					text( std::move( chain ), padding_, y_, theme().muted );
					y_ += height + style_->px( 3 );
				}

				std::vector<Pill> badges;
				if ( style_->show_tags )
				{
					termTags( entry, badges );
				}
				if ( style_->show_frequencies )
				{
					frequencies( entry, badges );
				}
				if ( !badges.empty() )
				{
					y_ += pills( badges, padding_ ) + style_->px( 4 );
				}

				if ( style_->show_pitch )
				{
					pitches( entry );
				}

				definitions( entry );
			}

			void termTags( const lookup::TermEntry& entry, std::vector<Pill>& out ) const
			{
				std::vector<std::string_view> seen;
				for ( const auto& definition : entry.definitions )
				{
					const auto& dictionary = *( *set_ )[definition.dictionary].dictionary;
					for ( const auto name : splitTags( dictionary.string( dictionary.terms()[definition.term].term_tags ) ) )
					{
						if ( std::ranges::contains( seen, name ) )
						{
							continue;
						}
						seen.push_back( name );
						const auto* tag = dictionary.findTag( name );
						out.push_back( { .label = std::string( name ), .color = theme().tag( tag != nullptr ? dictionary.string( tag->category ) : "" ) } );
					}
				}
			}

			void frequencies( const lookup::TermEntry& entry, std::vector<Pill>& out ) const
			{
				for ( std::size_t i = 0; i < entry.frequencies.size(); )
				{
					const auto  dictionary = entry.frequencies[i].dictionary;
					std::string values;
					for ( ; i < entry.frequencies.size() && entry.frequencies[i].dictionary == dictionary; ++i )
					{
						const auto& f = entry.frequencies[i];
						if ( !values.empty() )
						{
							values.append( ", " );
						}
						values.append( f.display.empty() ? std::to_string( f.value ) : std::string( f.display ) );
					}
					out.push_back( { .label = shortName( ( *set_ )[dictionary].name ), .color = theme().tag_frequency, .value = std::move( values ) } );
				}
			}

			void pitches( const lookup::TermEntry& entry )
			{
				for ( std::size_t i = 0; i < entry.pitches.size(); )
				{
					const auto  dictionary = entry.pitches[i].dictionary;
					std::string line;
					for ( ; i < entry.pitches.size() && entry.pitches[i].dictionary == dictionary; ++i )
					{
						const auto& p = entry.pitches[i];
						if ( !line.empty() )
						{
							line.append( "   " );
						}
						if ( p.position >= 0 )
						{
							line.append( std::format( "{} [{}]", pitchText( entry.reading, p.position ), p.position ) );
						}
						else
						{
							line.append( std::format( "{} [{}]", entry.reading, p.pattern ) );
						}
					}

					const double saved    = y_;
					const double used     = pills( { { .label = shortName( ( *set_ )[dictionary].name ), .color = theme().tag_pitch } }, padding_ );
					auto         notation = impl_->layout( impl_->body.get(), line, content_width_ - style_->px( 90 ) );
					const auto   height   = PopupRenderer::Impl::size( notation.get() ).second;
					text( std::move( notation ), padding_ + style_->px( 90 ), saved, theme().text );
					y_ = saved + std::max<double>( used, height ) + style_->px( 3 );
				}
			}

			[[nodiscard]] std::string glossaryMarkup( const dict::Dictionary& dictionary, const lookup::LoadedDictionary& loaded, const dict::format::TermRecord& term ) const
			{
				const auto                pill_fg = theme().pill_text.hex();
				const auto                pill_bg = theme().tag_part_of_speech.hex();
				const dict::MarkupOptions options{ .format = dict::Markup::Pango, .styles = loaded.styles.get(), .pill_foreground = pill_fg, .pill_background = pill_bg };

				const auto  glossary = dictionary.glossary( term );
				const auto  texts    = std::ranges::count( glossary, dict::format::GlossKind::Text, &dict::format::GlossRecord::kind );
				std::string markup;
				for ( const auto& gloss : glossary )
				{
					if ( !markup.empty() )
					{
						markup.push_back( '\n' );
					}
					if ( texts > 1 && gloss.kind == dict::format::GlossKind::Text )
					{
						markup.append( "• " );
					}
					dict::renderGlossary( markup, gloss.kind, dictionary.string( gloss.data ), options );
				}
				return markup;
			}

			void definitions( const lookup::TermEntry& entry )
			{
				const int   indent   = style_->px( 14 );
				std::size_t in_group = 0;
				std::size_t hidden   = 0;
				const auto  more     = [&] {
                    if ( hidden > 0 )
                    {
                        auto       layout = impl_->layout( impl_->italic.get(), std::format( "+ {} more", hidden ) );
                        const auto height = PopupRenderer::Impl::size( layout.get() ).second;
                        text( std::move( layout ), padding_ + indent, y_, theme().muted, false );
                        y_ += height + style_->px( 3 );
                        hidden = 0;
                    }
				};
				for ( std::size_t i = 0; i < entry.definitions.size(); ++i )
				{
					const auto& definition          = entry.definitions[i];
					const auto& loaded              = ( *set_ )[definition.dictionary];
					const auto& dictionary          = *loaded.dictionary;
					const auto& term                = dictionary.terms()[definition.term];
					const bool  first_of_dictionary = i == 0 || entry.definitions[i - 1].dictionary != definition.dictionary;
					if ( first_of_dictionary )
					{
						more();
						in_group = 0;
					}
					if ( style_->max_senses > 0 && std::cmp_greater_equal( in_group, style_->max_senses ) )
					{
						++hidden;
						continue;
					}
					++in_group;

					std::vector<Pill> tags;
					std::string       sense;
					if ( style_->show_tags )
					{
						for ( const auto name : splitTags( dictionary.string( term.definition_tags ) ) )
						{
							if ( isNumber( name ) )
							{
								sense = std::string( name );
								continue;
							}
							const auto* tag = dictionary.findTag( name );
							tags.push_back( { .label = std::string( name ), .color = theme().tag( tag != nullptr ? dictionary.string( tag->category ) : "" ) } );
						}
					}
					if ( first_of_dictionary )
					{
						y_ += i > 0 ? style_->px( 4 ) : 0;
						if ( style_->show_dictionary )
						{
							tags.push_back( { .label = shortName( loaded.name ), .color = theme().tag_dictionary } );
						}
					}

					double x = padding_;
					if ( !sense.empty() )
					{
						auto       number = impl_->layout( impl_->small.get(), sense + "." );
						const auto width  = PopupRenderer::Impl::size( number.get() ).first;
						text( std::move( number ), x, y_, theme().muted );
						x += width + style_->px( 5 );
					}
					if ( !tags.empty() )
					{
						y_ += pills( tags, x ) + style_->px( 3 );
					}

					const auto markup = glossaryMarkup( dictionary, loaded, term );
					if ( !markup.empty() )
					{
						auto       body   = impl_->layout( impl_->body.get(), markup, content_width_ - indent, true );
						const auto height = PopupRenderer::Impl::size( body.get() ).second;
						text( std::move( body ), padding_ + indent, y_, theme().text );
						y_ += height;
					}
					y_ += style_->px( 5 );
				}
				more();
			}

			void kanji( const lookup::KanjiEntry& entry )
			{
				const auto&  dictionary = *( *set_ )[entry.dictionary].dictionary;
				const auto&  record     = *entry.record;
				const double top        = y_;

				auto glyph          = impl_->layout( impl_->kanji.get(), dictionary.string( record.character ) );
				const auto [gw, gh] = PopupRenderer::Impl::size( glyph.get() );
				text( std::move( glyph ), padding_, y_, theme().text );

				const double column = padding_ + gw + style_->px( 14 );
				const int    width  = std::max( style_->px( 80 ), padding_ + content_width_ - static_cast<int>( column ) );

				std::string meanings;
				for ( const auto meaning : dictionary.stringList( record.meanings_begin, record.meanings_count ) )
				{
					if ( !meanings.empty() )
					{
						meanings.append( ", " );
					}
					meanings.append( dictionary.string( meaning ) );
				}
				auto line = [&]( const PangoFontDescription* font, const std::string& content, const Color& color, std::string_view label = {} ) {
					if ( content.empty() )
					{
						return;
					}
					// Readings are named in the friendly designs (On, Kun) instead of told apart by colour only.
					const bool named  = !label.empty() && !classic();
					auto       layout = named ? impl_->layout( font, std::format( "<span foreground=\"{}\">{}</span>  {}", theme().muted.hex(), label, escaped( content ) ), width, true )
					                          : impl_->layout( font, content, width );
					const auto height = PopupRenderer::Impl::size( layout.get() ).second;
					text( std::move( layout ), column, y_, color );
					y_ += height + style_->px( 3 );
				};
				line( impl_->bold.get(), meanings, theme().text );
				line( impl_->body.get(), std::string( dictionary.string( record.onyomi ) ), theme().text, "On" );
				line( impl_->body.get(), std::string( dictionary.string( record.kunyomi ) ), classic() ? theme().muted : theme().text, "Kun" );

				std::vector<Pill> stats;
				const auto        list = dictionary.stringList( record.stats_begin, record.stats_count * 2 );
				for ( std::size_t i = 0; i + 1 < list.size(); i += 2 )
				{
					const auto key = dictionary.string( list[i] );
					if ( key == "strokes" || key == "grade" || key == "jlpt" || key == "freq" )
					{
						const auto* tag = dictionary.findTag( key );
						stats.push_back(
								{ .label = std::string( tag != nullptr ? dictionary.string( tag->notes ) : key ),
						          .color = theme().tag_default,
						          .value = std::string( dictionary.string( list[i + 1] ) ) }
						);
					}
				}
				for ( const auto& f : entry.frequencies )
				{
					stats.push_back(
							{ .label = shortName( ( *set_ )[f.dictionary].name ),
					          .color = theme().tag_frequency,
					          .value = f.display.empty() ? std::to_string( f.value ) : std::string( f.display ) }
					);
				}
				const double saved_x = column;
				y_ += pills( stats, saved_x );
				y_ = std::max( y_, top + gh ) + style_->px( 8 );
			}

			// Character boxes of laid out text, for selecting it in the popup.
			void record( const TextOp& item )
			{
				const std::string_view text( pango_layout_get_text( item.layout.get() ) );
				const auto             offset = static_cast<std::uint32_t>( selectable_text_.size() );
				selectable_text_.append( text );
				for ( std::size_t at = 0; at < text.size(); )
				{
					const std::size_t start = at;
					( void )utf8::decode( text, at );
					if ( text[start] == '\n' )
					{
						continue;
					}
					PangoRectangle box{};
					pango_layout_index_to_pos( item.layout.get(), static_cast<int>( start ), &box );
					const double x = item.x + pango_units_to_double( std::min( box.x, box.x + box.width ) );
					const double y = item.y + pango_units_to_double( box.y );
					glyphs_.push_back( {
							.x      = static_cast<float>( x ),
							.y      = static_cast<float>( y ),
							.width  = static_cast<float>( pango_units_to_double( std::abs( box.width ) ) ),
							.height = static_cast<float>( pango_units_to_double( box.height ) ),
							.begin  = offset + static_cast<std::uint32_t>( start ),
							.end    = offset + static_cast<std::uint32_t>( at ),
					} );
				}
			}

			std::shared_ptr<const PopupImage> paint()
			{
				const int                                    height  = std::clamp( static_cast<int>( std::ceil( y_ ) ) + padding_, 1, max_content_height );
				auto*                                        surface = cairo_image_surface_create( CAIRO_FORMAT_ARGB32, width_, height );
				const std::unique_ptr<cairo_t, CairoDeleter> cr( cairo_create( surface ) );

				for ( const Op& op : ops_ )
				{
					if ( const auto* rect = std::get_if<RectOp>( &op ) )
					{
						setSource( cr.get(), rect->color );
						if ( rect->radius > 0.0 )
						{
							roundedRect( cr.get(), rect->x, rect->y, rect->width, rect->height, rect->radius, rect->round_left, rect->round_right );
						}
						else
						{
							cairo_rectangle( cr.get(), rect->x, rect->y, rect->width, rect->height );
						}
						cairo_fill( cr.get() );
					}
					else if ( const auto* item = std::get_if<TextOp>( &op ) )
					{
						if ( item->y > height )
						{
							continue;
						}
						setSource( cr.get(), item->color );
						cairo_move_to( cr.get(), item->x, item->y );
						pango_cairo_update_layout( cr.get(), item->layout.get() );
						pango_cairo_show_layout( cr.get(), item->layout.get() );
						if ( item->selectable )
						{
							record( *item );
						}
					}
					else if ( const auto* icon = std::get_if<IconOp>( &op ) )
					{
						drawIcon( cr.get(), *icon );
					}
				}
				cairo_surface_flush( surface );
				return std::make_shared<const PopupImage>( surface, width_, height, std::move( regions_ ), std::move( buttons_ ), std::move( selectable_text_ ), std::move( glyphs_ ) );
			}

			const PopupRenderer::Impl*      impl_;
			std::span<const NoteState>      notes_;
			const PopupStyle*               style_;
			const lookup::LookupResult*     result_;
			const lookup::DictionarySet*    set_;
			int                             padding_;
			int                             width_;
			int                             content_width_;
			double                          y_;
			double                          limit_;
			std::vector<Op>                 ops_;
			std::vector<PopupImage::Region> regions_;
			std::vector<PopupImage::Button> buttons_;
			std::string                     selectable_text_;
			std::vector<PopupImage::Glyph>  glyphs_;
		};

	} // namespace

	PopupRenderer::PopupRenderer() :
		impl_( std::make_unique<Impl>() )
	{
	}

	PopupRenderer::~PopupRenderer() = default;

	void PopupRenderer::setStyle( const PopupStyle& style )
	{
		impl_->style = style;
		impl_->applyStyle();
	}

	const PopupStyle& PopupRenderer::style() const noexcept
	{
		return impl_->style;
	}

	std::shared_ptr<const PopupImage> PopupRenderer::render( const lookup::LookupResult& result, std::span<const NoteState> notes, int limit )
	{
		if ( result.empty() || !result.dictionaries || ( result.terms.empty() && !impl_->style.show_kanji ) )
		{
			return nullptr;
		}
		Builder builder( *impl_, result, notes, limit );
		return builder.build();
	}

	void PopupRenderer::warmUp()
	{
		const auto layout = impl_->layout( impl_->headword.get(), "漢字かなカナ abc 123 ꜜ «" );
		( void )Impl::size( layout.get() );
		const auto body = impl_->layout( impl_->body.get(), "<b>warm</b> up", 200, true );
		( void )Impl::size( body.get() );
	}

	namespace
	{

		// A layout of its own font map, so these helpers can run on any thread.
		struct StandaloneLayout
		{
			StandaloneLayout( std::string_view text, double pixel_size, std::string_view family ) :
				map( pango_cairo_font_map_new() ),
				context( pango_font_map_create_context( map.get() ) ),
				layout( pango_layout_new( context.get() ) )
			{
				const FontPtr     font( pango_font_description_new() );
				const std::string families = fontFamilies( family );
				pango_font_description_set_family( font.get(), families.c_str() );
				pango_font_description_set_absolute_size( font.get(), pixel_size * PANGO_SCALE );
				pango_layout_set_font_description( layout.get(), font.get() );
				const std::string owned( text );
				pango_layout_set_text( layout.get(), owned.c_str(), -1 );
			}

			std::unique_ptr<PangoFontMap, FontMapDeleter> map;
			std::unique_ptr<PangoContext, ContextDeleter> context;
			LayoutPtr                                     layout;
		};

	} // namespace

	std::string fontFamilies( std::string_view families )
	{
		const std::string_view wanted = families.empty() ? default_family : families;
#ifdef _WIN32
		// Installed families, read once: fonts are installed rarely, and listing them takes a while.
		static const std::vector<std::string> installed = [] {
			std::vector<std::string> names;
			PangoFontMap*            map   = pango_cairo_font_map_new();
			PangoFontFamily**        list  = nullptr;
			int                      count = 0;
			pango_font_map_list_families( map, &list, &count );
			for ( int i = 0; i < count; ++i )
			{
				names.emplace_back( pango_font_family_get_name( list[i] ) );
			}
			g_free( list );
			g_object_unref( map );
			return names;
		}();
		const auto same = []( std::string_view a, std::string_view b ) {
			const auto lower = []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; };
			return a.size() == b.size() && std::ranges::equal( a, b, {}, lower, lower );
		};
		for ( const auto part : std::views::split( wanted, ',' ) )
		{
			std::string_view name( part.begin(), part.end() );
			while ( name.starts_with( ' ' ) )
			{
				name.remove_prefix( 1 );
			}
			while ( name.ends_with( ' ' ) )
			{
				name.remove_suffix( 1 );
			}
			if ( const auto it = std::ranges::find_if( installed, [&]( const std::string& known ) { return same( known, name ); } ); it != installed.end() )
			{
				return *it;
			}
			if ( same( name, "sans-serif" ) || same( name, "serif" ) || same( name, "monospace" ) )
			{
				return std::string( name );
			}
		}
		return "sans-serif";
#else
		return std::string( wanted );
#endif
	}

	TextRaster rasterizeText( std::string_view text, double pixel_size, std::string_view family )
	{
		const StandaloneLayout text_layout( text, pixel_size, family );
		int                    w = 0;
		int                    h = 0;
		pango_layout_get_pixel_size( text_layout.layout.get(), &w, &h );
		const int pad = static_cast<int>( std::lround( pixel_size / 2.0 ) );
		if ( w <= 0 || h <= 0 )
		{
			return {};
		}
		TextRaster       raster{ .width = w + ( 2 * pad ), .height = h + ( 2 * pad ) };
		cairo_surface_t* surface = cairo_image_surface_create( CAIRO_FORMAT_RGB24, raster.width, raster.height );
		{
			const std::unique_ptr<cairo_t, CairoDeleter> cr( cairo_create( surface ) );
			cairo_set_source_rgb( cr.get(), 1.0, 1.0, 1.0 );
			cairo_paint( cr.get() );
			cairo_set_source_rgb( cr.get(), 0.0, 0.0, 0.0 );
			cairo_move_to( cr.get(), pad, pad );
			pango_cairo_update_layout( cr.get(), text_layout.layout.get() );
			pango_cairo_show_layout( cr.get(), text_layout.layout.get() );
		}
		cairo_surface_flush( surface );
		const int            stride = cairo_image_surface_get_stride( surface );
		const unsigned char* data   = cairo_image_surface_get_data( surface );
		raster.pixels.resize( static_cast<std::size_t>( raster.width ) * static_cast<std::size_t>( raster.height ) );
		for ( int y = 0; y < raster.height; ++y )
		{
			const auto* row = reinterpret_cast<const std::uint32_t*>( data + ( static_cast<std::ptrdiff_t>( y ) * stride ) );
			for ( int x = 0; x < raster.width; ++x )
			{
				raster.pixels[( static_cast<std::size_t>( y ) * static_cast<std::size_t>( raster.width ) ) + static_cast<std::size_t>( x )] = row[x] & 0xFFFFFFU;
			}
		}
		cairo_surface_destroy( surface );
		return raster;
	}

	FontCoverage checkFont( std::string_view text, std::string_view family )
	{
		const StandaloneLayout text_layout( text, 16.0, family );
		FontCoverage           coverage;
		coverage.missing      = pango_layout_get_unknown_glyphs_count( text_layout.layout.get() );
		PangoLayoutIter* iter = pango_layout_get_iter( text_layout.layout.get() );
		if ( const PangoLayoutRun* run = pango_layout_iter_get_run_readonly( iter ); run != nullptr && run->item != nullptr && run->item->analysis.font != nullptr )
		{
			const FontPtr described( pango_font_describe( run->item->analysis.font ) );
			if ( const char* name = pango_font_description_get_family( described.get() ); name != nullptr )
			{
				coverage.family = name;
			}
		}
		pango_layout_iter_free( iter );
		return coverage;
	}

	PopupSize popupSize( const PopupImage& content, const PopupStyle& style ) noexcept
	{
		return { .width = content.width(), .height = std::min( content.height(), style.px( style.max_height ) ) };
	}

	void composePopup( cairo_t* cr, const PopupImage& content, int scroll, PopupSize size, const PopupStyle& style, std::string_view badge )
	{
		const Theme& theme  = style.theme;
		const double w      = size.width;
		const double h      = size.height;
		const double radius = style.rounded ? style.px( style.corner_radius ) : 0.0;
		const double line   = style.border_width > 0.0 ? std::max( 1.0, std::round( style.border_width * style.scale ) ) : 0.0;

		cairo_save( cr );
		cairo_set_operator( cr, CAIRO_OPERATOR_SOURCE );
		cairo_set_source_rgba( cr, 0.0, 0.0, 0.0, 0.0 );
		cairo_paint( cr );
		cairo_set_operator( cr, CAIRO_OPERATOR_OVER );

		roundedRect( cr, 0.0, 0.0, w, h, radius );
		// A see-through background needs a compositor to blend it; without one it would show as black.
		Color background = theme.background;
		background.a *= style.rounded ? std::clamp( style.opacity, 0.3, 1.0 ) : 1.0;
		setSource( cr, background );
		cairo_fill_preserve( cr );
		cairo_clip( cr );

		cairo_set_source_surface( cr, content.surface(), 0.0, -static_cast<double>( scroll ) );
		cairo_paint( cr );

		if ( content.height() > size.height )
		{
			const double track = h - ( 2.0 * radius );
			const double thumb = std::max( static_cast<double>( style.px( 24 ) ), track * h / content.height() );
			const double range = std::max( 1, content.height() - size.height );
			const double top   = radius + ( ( track - thumb ) * scroll / range );
			roundedRect( cr, w - style.px( 6 ), top, style.px( 3 ), thumb, style.px( 1.5 ) );
			setSource( cr, theme.scrollbar );
			cairo_fill( cr );
		}

		if ( !badge.empty() )
		{
			// Laid out with Pango like the rest: it takes glyphs the first font lacks (✓) from others.
			PangoLayout*          layout = pango_cairo_create_layout( cr );
			PangoFontDescription* font   = pango_font_description_new();
			pango_font_description_set_family( font, fontFamilies( style.font_family ).c_str() );
			pango_font_description_set_weight( font, PANGO_WEIGHT_BOLD );
			pango_font_description_set_absolute_size( font, style.px( style.font_size * 0.85 ) * PANGO_SCALE );
			pango_layout_set_font_description( layout, font );
			pango_font_description_free( font );
			pango_layout_set_text( layout, badge.data(), static_cast<int>( badge.size() ) );
			int text_w = 0;
			int text_h = 0;
			pango_layout_get_pixel_size( layout, &text_w, &text_h );
			const double pad_x = style.px( 6 );
			const double pad_y = style.px( 4 );
			const double bw    = text_w + ( 2 * pad_x );
			const double bh    = text_h + ( 2 * pad_y );
			const double bx    = w - bw - style.px( 12 );
			const double by    = style.px( 8 );
			roundedRect( cr, bx, by, bw, bh, style.px( 4 ) );
			setSource( cr, theme.tag_frequency );
			cairo_fill( cr );
			setSource( cr, theme.pill_text );
			cairo_move_to( cr, bx + pad_x, by + pad_y );
			pango_cairo_show_layout( cr, layout );
			g_object_unref( layout );
		}

		cairo_reset_clip( cr );
		if ( line > 0.0 )
		{
			roundedRect( cr, line / 2.0, line / 2.0, w - line, h - line, std::max( 0.0, radius - ( line / 2.0 ) ) );
			setSource( cr, theme.border );
			cairo_set_line_width( cr, line );
			cairo_stroke( cr );
		}
		cairo_restore( cr );
	}

	void drawSelection( cairo_t* cr, const PopupImage& content, std::size_t first, std::size_t last, int scroll, PopupSize size, const PopupStyle& style )
	{
		const auto& glyphs = content.glyphs();
		if ( glyphs.empty() )
		{
			return;
		}
		if ( first > last )
		{
			std::swap( first, last );
		}
		last = std::min( last, glyphs.size() - 1 );
		cairo_save( cr );
		cairo_rectangle( cr, 0.0, 0.0, size.width, size.height );
		cairo_clip( cr );
		setSource( cr, style.theme.selection );
		for ( std::size_t i = first; i <= last; ++i )
		{
			const auto& g = glyphs[i];
			cairo_rectangle( cr, g.x, g.y - static_cast<float>( scroll ), g.width, g.height );
		}
		cairo_fill( cr );
		cairo_restore( cr );
	}

} // namespace lexiglance::render
