#include <lexiglance/dictionary/StructuredContent.h>

#include <lexiglance/core/Json.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace lexiglance::dict
{

	namespace
	{

		constexpr std::uint8_t prop_foreground = 1U << 0U;
		constexpr std::uint8_t prop_background = 1U << 1U;
		constexpr std::uint8_t prop_scale      = 1U << 2U;
		constexpr std::uint8_t prop_bold       = 1U << 3U;
		constexpr std::uint8_t prop_italic     = 1U << 4U;
		constexpr std::uint8_t prop_underline  = 1U << 5U;
		constexpr std::uint8_t prop_strike     = 1U << 6U;
		constexpr std::uint8_t prop_valign     = 1U << 7U;

		std::string_view trim( std::string_view text ) noexcept
		{
			const auto begin = text.find_first_not_of( " \t\r\n" );
			if ( begin == std::string_view::npos )
			{
				return {};
			}
			const auto end = text.find_last_not_of( " \t\r\n" );
			return text.substr( begin, end - begin + 1 );
		}

		std::string_view unquote( std::string_view text ) noexcept
		{
			text = trim( text );
			if ( text.size() >= 2 && ( text.front() == '"' || text.front() == '\'' ) && text.back() == text.front() )
			{
				return text.substr( 1, text.size() - 2 );
			}
			return text;
		}

		// Only plain colours survive; CSS functions and variables cannot be expressed in Pango or Qt rich text.
		bool isColor( std::string_view value ) noexcept
		{
			if ( value.starts_with( '#' ) )
			{
				return ( value.size() == 4 || value.size() == 7 || value.size() == 9 ) &&
				       std::ranges::all_of( value.substr( 1 ), []( char c ) { return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ); } );
			}
			return !value.empty() && std::ranges::all_of( value, []( char c ) { return c >= 'a' && c <= 'z'; } );
		}

		float parseScale( std::string_view value ) noexcept
		{
			value = trim( value );
			if ( value == "xx-small" )
			{
				return 0.6F;
			}
			if ( value == "x-small" )
			{
				return 0.75F;
			}
			if ( value == "small" || value == "smaller" )
			{
				return 0.85F;
			}
			if ( value == "large" || value == "larger" )
			{
				return 1.2F;
			}
			if ( value == "x-large" )
			{
				return 1.5F;
			}

			float      number = 0.0F;
			const auto parsed = std::from_chars( value.data(), value.data() + value.size(), number );
			if ( parsed.ec != std::errc{} || number <= 0.0F )
			{
				return 1.0F;
			}
			const std::string_view unit( parsed.ptr, value.data() + value.size() );
			if ( unit == "%" )
			{
				return number / 100.0F;
			}
			if ( unit == "em" || unit == "rem" )
			{
				return number;
			}
			return 1.0F;
		}

		std::int8_t verticalAlign( std::string_view value ) noexcept
		{
			if ( value == "super" )
			{
				return 1;
			}
			return value == "sub" ? -1 : 0;
		}

		void applyProperty( std::string_view name, std::string_view value, TextStyle& style, std::uint16_t& set )
		{
			value = trim( value );
			if ( name == "color" && isColor( value ) )
			{
				style.foreground = std::string( value );
				set |= prop_foreground;
			}
			else if ( ( name == "background-color" || name == "background" || name == "backgroundColor" ) && isColor( value ) )
			{
				style.background = std::string( value );
				set |= prop_background;
			}
			else if ( name == "font-size" || name == "fontSize" )
			{
				style.scale = parseScale( value );
				set |= prop_scale;
			}
			else if ( name == "font-weight" || name == "fontWeight" )
			{
				int weight = 0;
				( void )std::from_chars( value.data(), value.data() + value.size(), weight );
				style.bold = value == "bold" || value == "bolder" || weight >= 600;
				set |= prop_bold;
			}
			else if ( name == "font-style" || name == "fontStyle" )
			{
				style.italic = value == "italic" || value == "oblique";
				set |= prop_italic;
			}
			else if ( name == "text-decoration" || name == "text-decoration-line" || name == "textDecorationLine" )
			{
				style.underline = value.contains( "underline" );
				style.strike    = value.contains( "line-through" );
				set |= prop_underline | prop_strike;
			}
			else if ( name == "vertical-align" || name == "verticalAlign" )
			{
				style.valign = verticalAlign( value );
				set |= prop_valign;
			}
		}

		void merge( TextStyle& target, const TextStyle& source, std::uint16_t set )
		{
			if ( ( set & prop_foreground ) != 0U )
			{
				target.foreground = source.foreground;
			}
			if ( ( set & prop_background ) != 0U )
			{
				target.background = source.background;
			}
			if ( ( set & prop_scale ) != 0U )
			{
				target.scale = source.scale;
			}
			if ( ( set & prop_bold ) != 0U )
			{
				target.bold = source.bold;
			}
			if ( ( set & prop_italic ) != 0U )
			{
				target.italic = source.italic;
			}
			if ( ( set & prop_underline ) != 0U )
			{
				target.underline = source.underline;
			}
			if ( ( set & prop_strike ) != 0U )
			{
				target.strike = source.strike;
			}
			if ( ( set & prop_valign ) != 0U )
			{
				target.valign = source.valign;
			}
		}

		bool isBlock( std::string_view tag ) noexcept
		{
			static constexpr std::array<std::string_view, 11> blocks{ "div", "ul", "ol", "li", "table", "thead", "tbody", "tfoot", "tr", "details", "summary" };
			return std::ranges::find( blocks, tag ) != blocks.end();
		}

		std::string_view htmlTag( std::string_view tag ) noexcept
		{
			static constexpr std::array<std::string_view, 21> allowed{ "ruby", "rt", "rp", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "span", "div", "ol", "ul", "li", "details", "summary", "a", "sub", "sup", "br" };
			return std::ranges::find( allowed, tag ) != allowed.end() ? tag : std::string_view( "span" );
		}

		struct ListState
		{
			bool        ordered = false;
			int         counter = 0;
			std::string marker;
		};

		class Writer
		{
		public:
			Writer( std::string& out, const MarkupOptions& options ) :
				out_( &out ),
				options_( options )
			{
			}

			void node( const json::Value& value )
			{
				if ( value.isString() )
				{
					text( value.asString() );
				}
				else if ( value.isArray() )
				{
					for ( const json::Value& child : value.items() )
					{
						node( child );
					}
				}
				else if ( value.isObject() )
				{
					element( value );
				}
			}

			void text( std::string_view content )
			{
				if ( content.empty() )
				{
					return;
				}
				escapeMarkup( *out_, content, options_.format );
				line_start_   = content.back() == '\n';
				after_marker_ = false;
			}

			void finish()
			{
				while ( !out_->empty() && out_->back() == '\n' )
				{
					out_->pop_back();
				}
			}

		private:
			void newline()
			{
				out_->append( options_.format == Markup::Html ? "<br>" : "\n" );
				line_start_   = true;
				after_marker_ = false;
			}

			// A block directly after a list marker stays on the marker's line.
			void block()
			{
				if ( !line_start_ && !after_marker_ )
				{
					newline();
				}
			}

			[[nodiscard]] TextStyle styleOf( const json::Value& element, std::string_view tag, std::string_view data_content, std::string_view data_class ) const
			{
				TextStyle style;
				if ( options_.styles != nullptr )
				{
					options_.styles->apply( tag, data_content, data_class, style );
				}

				TextStyle     inline_style;
				std::uint16_t set = 0;
				for ( const json::Member& property : element["style"].members() )
				{
					if ( property.value.isString() )
					{
						applyProperty( property.key, property.value.asString(), inline_style, set );
					}
					else if ( property.value.isArray() && property.key == "textDecorationLine" )
					{
						std::string joined;
						for ( const json::Value& item : property.value.items() )
						{
							joined.append( item.asString() ).push_back( ' ' );
						}
						applyProperty( property.key, joined, inline_style, set );
					}
				}
				merge( style, inline_style, set );

				if ( tag == "th" )
				{
					style.bold = true;
				}
				if ( tag == "sup" )
				{
					style.valign = 1;
				}
				if ( tag == "sub" )
				{
					style.valign = -1;
				}
				if ( data_class == "tag" )
				{
					style.pill = true;
				}
				return style;
			}

			void open( const TextStyle& style )
			{
				if ( style.plain() || options_.format == Markup::Plain )
				{
					return;
				}

				const std::string_view fg    = style.pill && style.foreground.empty() ? options_.pill_foreground : std::string_view( style.foreground );
				const std::string_view bg    = style.pill && style.background.empty() ? options_.pill_background : std::string_view( style.background );
				const bool             bold  = style.bold || style.pill;
				const float            scale = style.pill && style.scale == 1.0F ? 0.8F : style.scale;

				if ( options_.format == Markup::Pango )
				{
					out_->append( "<span" );
					if ( bold )
					{
						out_->append( " weight=\"bold\"" );
					}
					if ( style.italic )
					{
						out_->append( " style=\"italic\"" );
					}
					if ( scale != 1.0F )
					{
						std::format_to( std::back_inserter( *out_ ), " size=\"{}%\"", static_cast<int>( scale * 100.0F ) );
					}
					if ( !fg.empty() )
					{
						std::format_to( std::back_inserter( *out_ ), " foreground=\"{}\"", fg );
					}
					if ( !bg.empty() )
					{
						std::format_to( std::back_inserter( *out_ ), " background=\"{}\"", bg );
					}
					if ( style.underline )
					{
						out_->append( " underline=\"single\"" );
					}
					if ( style.strike )
					{
						out_->append( " strikethrough=\"true\"" );
					}
					if ( style.valign != 0 )
					{
						out_->append( style.valign > 0 ? R"( baseline_shift="superscript" font_scale="superscript")" : R"( baseline_shift="subscript" font_scale="subscript")" );
					}
					out_->append( style.pill ? "> " : ">" );
					return;
				}

				std::string css;
				if ( bold )
				{
					css.append( "font-weight:bold;" );
				}
				if ( style.italic )
				{
					css.append( "font-style:italic;" );
				}
				if ( scale != 1.0F )
				{
					std::format_to( std::back_inserter( css ), "font-size:{}%;", static_cast<int>( scale * 100.0F ) );
				}
				if ( !fg.empty() )
				{
					std::format_to( std::back_inserter( css ), "color:{};", fg );
				}
				if ( !bg.empty() )
				{
					std::format_to( std::back_inserter( css ), "background-color:{};", bg );
				}
				if ( style.underline || style.strike )
				{
					css.append( style.underline ? "text-decoration:underline;" : "text-decoration:line-through;" );
				}
				if ( style.valign != 0 )
				{
					css.append( style.valign > 0 ? "vertical-align:super;" : "vertical-align:sub;" );
				}
				std::format_to( std::back_inserter( *out_ ), "<span style=\"{}\">{}", css, style.pill ? "&nbsp;" : "" );
			}

			void close( const TextStyle& style )
			{
				if ( style.plain() || options_.format == Markup::Plain )
				{
					return;
				}
				if ( options_.format == Markup::Pango )
				{
					out_->append( style.pill ? " </span> " : "</span>" );
				}
				else
				{
					out_->append( style.pill ? "&nbsp;</span> " : "</span>" );
				}
			}

			void children( const json::Value& element )
			{
				node( element["content"] );
			}

			void element( const json::Value& element )
			{
				const auto tag          = element["tag"].asString();
				const auto data_content = element["data"]["content"].asString();
				const auto data_class   = element["data"]["class"].asString();

				if ( options_.skip_attribution && data_content == "attribution" )
				{
					return;
				}
				if ( tag == "br" )
				{
					newline();
					return;
				}
				if ( tag == "img" )
				{
					text( element["title"].asString( element["alt"].asString() ) );
					return;
				}
				if ( tag == "rp" )
				{
					return;
				}

				const TextStyle style = styleOf( element, tag, data_content, data_class );
				if ( options_.format == Markup::Html && tag != "rt" )
				{
					htmlElement( element, tag, style );
					return;
				}
				// Collapsed sections (Wiktionary's grammar, etymology and examples) stay closed, as in Yomitan: the popup
				// cannot open them.
				if ( tag == "details" && !element["open"].asBool() )
				{
					return;
				}

				if ( tag == "ul" || tag == "ol" )
				{
					const auto type = unquote( element["style"]["listStyleType"].asString() );
					ListState  list{ .ordered = tag == "ol" };
					if ( type == "none" )
					{
						list.marker.clear();
					}
					else if ( type == "circle" )
					{
						list.marker = "◦ ";
					}
					else if ( type == "square" )
					{
						list.marker = "▪ ";
					}
					else if ( !type.empty() && type != "disc" && type != "decimal" )
					{
						list.marker = std::string( type );
					}
					else
					{
						list.marker = "• ";
					}

					block();
					lists_.push_back( std::move( list ) );
					open( style );
					children( element );
					close( style );
					lists_.pop_back();
					block();
					return;
				}

				if ( tag == "li" )
				{
					// The first item of a list nested right after a marker shares that marker's line, unindented.
					const bool marker_line = after_marker_;
					block();
					if ( !lists_.empty() )
					{
						ListState& list = lists_.back();
						for ( std::size_t i = 1; !marker_line && i < lists_.size(); ++i )
						{
							out_->append( " " );
						}
						if ( list.ordered )
						{
							text( std::format( "{}. ", ++list.counter ) );
						}
						else
						{
							text( list.marker );
						}
						after_marker_ = !list.marker.empty() || list.ordered;
					}
					open( style );
					children( element );
					close( style );
					block();
					return;
				}

				if ( tag == "tr" )
				{
					block();
					cell_ = 0;
					children( element );
					block();
					return;
				}

				if ( tag == "td" || tag == "th" )
				{
					if ( cell_++ > 0 )
					{
						text( "  " );
					}
					open( style );
					children( element );
					close( style );
					return;
				}

				if ( tag == "rt" )
				{
					TextStyle reading = style;
					reading.scale     = 0.7F;
					open( reading );
					text( "(" );
					children( element );
					text( ")" );
					close( reading );
					return;
				}

				const bool is_block = isBlock( tag );
				if ( is_block )
				{
					block();
				}
				open( style );
				children( element );
				close( style );
				if ( is_block )
				{
					block();
				}
			}

			void
			htmlElement( const json::Value& element, std::string_view tag, const TextStyle& style )
			{
				const auto name = htmlTag( tag );
				out_->push_back( '<' );
				out_->append( name );
				if ( name == "a" )
				{
					out_->append( " href=\"" );
					escapeMarkup( *out_, element["href"].asString(), Markup::Html );
					out_->push_back( '"' );
				}
				if ( const auto title = element["title"].asString(); !title.empty() )
				{
					out_->append( " title=\"" );
					escapeMarkup( *out_, title, Markup::Html );
					out_->push_back( '"' );
				}
				out_->push_back( '>' );
				open( style );
				children( element );
				close( style );
				std::format_to( std::back_inserter( *out_ ), "</{}>", name );
				line_start_ = isBlock( tag );
			}

			std::string*           out_;
			MarkupOptions          options_;
			std::vector<ListState> lists_;
			bool                   line_start_   = true;
			bool                   after_marker_ = false;
			int                    cell_         = 0;
		};

	} // namespace

	bool TextStyle::plain() const noexcept
	{
		return foreground.empty() && background.empty() && scale == 1.0F && !bold && !italic && !underline && !strike && !pill && valign == 0;
	}

	StyleSheet StyleSheet::parse( std::string_view css )
	{
		// Strip comments first so that braces inside them cannot confuse the block scanner.
		std::string clean;
		clean.reserve( css.size() );
		for ( std::size_t i = 0; i < css.size(); ++i )
		{
			if ( css.substr( i, 2 ) == "/*" )
			{
				const auto end = css.find( "*/", i + 2 );
				if ( end == std::string_view::npos )
				{
					break;
				}
				i = end + 1;
				continue;
			}
			clean.push_back( css[i] );
		}

		StyleSheet             sheet;
		const std::string_view text = clean;
		std::size_t            pos  = 0;
		while ( pos < text.size() )
		{
			const auto open = text.find( '{', pos );
			if ( open == std::string_view::npos )
			{
				break;
			}

			std::size_t close = open + 1;
			for ( int depth = 1; close < text.size() && depth > 0; ++close )
			{
				if ( text[close] == '{' )
				{
					++depth;
				}
				else if ( text[close] == '}' )
				{
					--depth;
				}
			}
			const auto selectors = text.substr( pos, open - pos );
			const auto body      = text.substr( open + 1, close - open - 2 );
			pos                  = close;

			// Top-level declarations only; nested blocks belong to descendant selectors we do not support.
			TextStyle     style;
			std::uint16_t set   = 0;
			int           depth = 0;
			std::size_t   start = 0;
			for ( std::size_t i = 0; i <= body.size(); ++i )
			{
				const char c = i < body.size() ? body[i] : ';';
				if ( c == '{' )
				{
					++depth;
				}
				else if ( c == '}' )
				{
					--depth;
					start = i + 1;
				}
				else if ( c == ';' && depth == 0 )
				{
					const auto declaration = body.substr( start, i - start );
					const auto colon       = declaration.find( ':' );
					if ( colon != std::string_view::npos && !declaration.contains( '&' ) )
					{
						applyProperty( trim( declaration.substr( 0, colon ) ), declaration.substr( colon + 1 ), style, set );
					}
					start = i + 1;
				}
			}
			if ( set == 0 )
			{
				continue;
			}

			std::size_t from = 0;
			while ( from <= selectors.size() )
			{
				auto comma = selectors.find( ',', from );
				if ( comma == std::string_view::npos )
				{
					comma = selectors.size();
				}
				const auto selector = trim( selectors.substr( from, comma - from ) );
				from                = comma + 1;

				const auto bracket = selector.find( '[' );
				if ( bracket == std::string_view::npos || selector.back() != ']' || selector.find_first_of( " >+~:" ) != std::string_view::npos ||
				     selector.find( '[', bracket + 1 ) != std::string_view::npos )
				{
					continue;
				}
				const auto inner = selector.substr( bracket + 1, selector.size() - bracket - 2 );
				const auto eq    = inner.find( '=' );
				if ( eq == std::string_view::npos )
				{
					continue;
				}
				const auto attribute = inner.substr( 0, eq );
				if ( attribute != "data-sc-content" && attribute != "data-sc-class" )
				{
					continue;
				}
				sheet.rules_.push_back(
						{
								.tag       = std::string( selector.substr( 0, bracket ) ),
								.attribute = std::string( attribute ),
								.value     = std::string( unquote( inner.substr( eq + 1 ) ) ),
								.style     = style,
								.set       = set,
						}
				);
			}
		}
		return sheet;
	}

	void StyleSheet::apply( std::string_view tag, std::string_view data_content, std::string_view data_class, TextStyle& style ) const
	{
		for ( const Rule& rule : rules_ )
		{
			if ( !rule.tag.empty() && rule.tag != tag )
			{
				continue;
			}
			const auto actual = rule.attribute == "data-sc-content" ? data_content : data_class;
			if ( !actual.empty() && actual == rule.value )
			{
				merge( style, rule.style, rule.set );
			}
		}
	}

	void escapeMarkup( std::string& out, std::string_view text, Markup format )
	{
		if ( format == Markup::Plain )
		{
			out.append( text );
			return;
		}
		for ( const char c : text )
		{
			switch ( c )
			{
				case '&':
					out.append( "&amp;" );
					break;
				case '<':
					out.append( "&lt;" );
					break;
				case '>':
					out.append( "&gt;" );
					break;
				case '"':
					out.append( "&quot;" );
					break;
				case '\'':
					out.append( "&#39;" );
					break;
				case '\n':
					out.append( format == Markup::Html ? "<br>" : "\n" );
					break;
				default:
					out.push_back( c );
			}
		}
	}

	void renderGlossary( std::string& out, format::GlossKind kind, std::string_view data, const MarkupOptions& options )
	{
		using format::GlossKind;

		if ( kind == GlossKind::Text )
		{
			escapeMarkup( out, data, options.format );
			return;
		}

		auto document = json::Document::parse( std::string( data ) );
		if ( !document )
		{
			return;
		}
		const json::Value& root = document->root();

		switch ( kind )
		{
			case GlossKind::StructuredContent:
			{
				Writer writer( out, options );
				writer.node( root );
				writer.finish();
				break;
			}
			case GlossKind::Image:
			{
				const auto label = root["description"].asString( root["title"].asString( root["alt"].asString() ) );
				escapeMarkup( out, label.empty() ? std::string_view( "[image]" ) : label, options.format );
				break;
			}
			case GlossKind::Deinflection:
			{
				// A form of another word: "-> книга (genitive singular)".
				escapeMarkup( out, "-> ", options.format );
				escapeMarkup( out, root[0].asString(), options.format );
				std::string rules;
				for ( const json::Value& rule : root[1].items() )
				{
					if ( !rule.asString().empty() )
					{
						rules.append( rules.empty() ? "" : ", " ).append( rule.asString() );
					}
				}
				if ( !rules.empty() )
				{
					escapeMarkup( out, " (" + rules + ")", options.format );
				}
				break;
			}
			case GlossKind::Text:
				break;
		}
	}

} // namespace lexiglance::dict
