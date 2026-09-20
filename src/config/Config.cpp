#include <lexiglance/config/Config.h>

#include <lexiglance/config/Keys.h>
#include <lexiglance/core/Json.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lexiglance::config
{

	namespace
	{

		void readInt( const json::Value& object, std::string_view key, int& target, int min, int max )
		{
			if ( const auto* value = object.find( key ); value != nullptr && value->isNumber() )
			{
				target = static_cast<int>( std::clamp<std::int64_t>( value->asInt(), min, max ) );
			}
		}

		void readBool( const json::Value& object, std::string_view key, bool& target )
		{
			if ( const auto* value = object.find( key ); value != nullptr && value->isBool() )
			{
				target = value->asBool();
			}
		}

		void readString( const json::Value& object, std::string_view key, std::string& target )
		{
			if ( const auto* value = object.find( key ); value != nullptr && value->isString() )
			{
				target = std::string( value->asString() );
			}
		}

		void readStrings( const json::Value& object, std::string_view key, std::vector<std::string>& target )
		{
			if ( const auto* value = object.find( key ); value != nullptr && value->isArray() )
			{
				target.clear();
				for ( const json::Value& item : value->items() )
				{
					if ( item.isString() )
					{
						target.emplace_back( item.asString() );
					}
				}
			}
		}

		std::string_view selectionName( SelectionMode mode )
		{
			switch ( mode )
			{
				case SelectionMode::Always:
					return "always";
				case SelectionMode::WithTrigger:
					return "with-trigger";
				case SelectionMode::Off:
					break;
			}
			return "off";
		}

		OcrEngine parseEngine( std::string_view name )
		{
			if ( name == "paddle" )
			{
				return OcrEngine::Paddle;
			}
			if ( name == "tesseract" )
			{
				return OcrEngine::Tesseract;
			}
			return OcrEngine::Auto;
		}

		const char* engineName( OcrEngine engine )
		{
			switch ( engine )
			{
				case OcrEngine::Paddle:
					return "paddle";
				case OcrEngine::Tesseract:
					return "tesseract";
				case OcrEngine::Auto:
					break;
			}
			return "auto";
		}

		OcrMode parseOcr( std::string_view name )
		{
			if ( name == "off" )
			{
				return OcrMode::Off;
			}
			if ( name == "always" )
			{
				return OcrMode::Always;
			}
			return OcrMode::Fallback;
		}

		std::string_view ocrName( OcrMode mode )
		{
			switch ( mode )
			{
				case OcrMode::Off:
					return "off";
				case OcrMode::Always:
					return "always";
				case OcrMode::Fallback:
					break;
			}
			return "fallback";
		}

		SelectionMode parseSelection( std::string_view name )
		{
			if ( name == "always" )
			{
				return SelectionMode::Always;
			}
			if ( name == "with-trigger" )
			{
				return SelectionMode::WithTrigger;
			}
			return SelectionMode::Off;
		}

		Theme parseTheme( std::string_view name )
		{
			if ( name == "light" )
			{
				return Theme::Light;
			}
			if ( name == "dark" )
			{
				return Theme::Dark;
			}
			return Theme::Auto;
		}

		Compositor parseCompositor( std::string_view name )
		{
			if ( name == "on" )
			{
				return Compositor::On;
			}
			if ( name == "off" )
			{
				return Compositor::Off;
			}
			return Compositor::Auto;
		}

		std::string_view compositorName( Compositor compositor )
		{
			switch ( compositor )
			{
				case Compositor::On:
					return "on";
				case Compositor::Off:
					return "off";
				case Compositor::Auto:
					break;
			}
			return "auto";
		}

		// Enum values and their names in the file, looked up both ways.
		template <typename Enum, std::size_t N>
		struct Names
		{
			std::array<std::pair<Enum, std::string_view>, N> entries;

			[[nodiscard]] Enum parse( std::string_view name, Enum fallback ) const
			{
				const auto it = std::ranges::find( entries, name, &std::pair<Enum, std::string_view>::second );
				return it != entries.end() ? it->first : fallback;
			}

			[[nodiscard]] std::string_view name( Enum value ) const
			{
				const auto it = std::ranges::find( entries, value, &std::pair<Enum, std::string_view>::first );
				return it != entries.end() ? it->second : entries.front().second;
			}
		};

		constexpr Names<HighlightStyle, 7> highlight_names{ { {
				{ HighlightStyle::Underline, "underline" },
				{ HighlightStyle::Outline, "outline" },
				{ HighlightStyle::Fill, "fill" },
				{ HighlightStyle::DoubleUnderline, "double-underline" },
				{ HighlightStyle::DottedUnderline, "dotted-underline" },
				{ HighlightStyle::WavyUnderline, "wavy-underline" },
				{ HighlightStyle::Brackets, "brackets" },
		} } };

		constexpr Names<PopupDesign, 3> design_names{ { {
				{ PopupDesign::Friendly, "friendly" },
				{ PopupDesign::Classic, "classic" },
				{ PopupDesign::Compact, "compact" },
		} } };

		constexpr Names<ColorScheme, 7> scheme_names{ { {
				{ ColorScheme::Default, "default" },
				{ ColorScheme::Paper, "paper" },
				{ ColorScheme::Nord, "nord" },
				{ ColorScheme::Sakura, "sakura" },
				{ ColorScheme::Matcha, "matcha" },
				{ ColorScheme::Midnight, "midnight" },
				{ ColorScheme::Contrast, "contrast" },
		} } };

		constexpr Names<PopupPlacement, 2> placement_names{ { {
				{ PopupPlacement::BelowText, "below" },
				{ PopupPlacement::AboveText, "above" },
		} } };

		HighlightStyle parseHighlight( std::string_view name )
		{
			return highlight_names.parse( name, HighlightStyle::Underline );
		}

		std::string_view highlightName( HighlightStyle style )
		{
			return highlight_names.name( style );
		}

		// A colour as "#rrggbb" (or "#rrggbbaa"); anything else is dropped so the scheme's colour applies.
		void readColor( const json::Value& object, std::string_view key, std::string& target )
		{
			std::string value;
			readString( object, key, value );
			const bool hex = ( value.size() == 7 || value.size() == 9 ) && value.front() == '#' &&
			                 std::ranges::all_of( std::string_view( value ).substr( 1 ), []( char c ) { return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ); } );
			target         = hex ? std::move( value ) : std::string();
		}

		std::string_view themeName( Theme theme )
		{
			switch ( theme )
			{
				case Theme::Light:
					return "light";
				case Theme::Dark:
					return "dark";
				case Theme::Auto:
					break;
			}
			return "auto";
		}

		void writeStrings( json::Writer& out, std::string_view key, const std::vector<std::string>& values )
		{
			out.key( key ).beginArray();
			for ( const auto& value : values )
			{
				out.value( value );
			}
			out.endArray();
		}

	} // namespace

	Result<Config> Config::fromJson( const json::Value& root )
	{
		Config config;
		if ( !root.isObject() )
		{
			return fail( "configuration must be a JSON object" );
		}

		readString( root, "language", config.language );
		readStrings( root, "disabled_languages", config.disabled_languages );
		readString( root, "log_level", config.log_level );
		readBool( root, "paused", config.paused );
		readBool( root, "statistics", config.statistics );
		readBool( root, "statistics_translations", config.statistics_translations );

		const json::Value& scan = root["scan"];
		readStrings( scan, "trigger", config.scan.trigger );
		readInt( scan, "max_length", config.scan.max_length, 1, 64 );
		readInt( scan, "delay_ms", config.scan.delay_ms, 0, 1000 );
		readInt( scan, "move_threshold", config.scan.move_threshold, 0, 100 );
		readBool( scan, "hide_on_no_result", config.scan.hide_on_no_result );
		readBool( scan, "search_kanji", config.scan.search_kanji );
		readBool( scan, "highlight", config.scan.highlight );
		readBool( scan, "accessibility", config.scan.accessibility );
		readStrings( scan, "ignored_windows", config.scan.ignored_windows );
		readStrings( scan, "ocr_windows", config.scan.ocr_windows );
		readBool( scan, "ocr_vertical", config.scan.ocr_vertical );
		readString( scan, "ocr_model", config.scan.ocr_model );
		if ( config.scan.ocr_model != "best" )
		{
			config.scan.ocr_model = "fast";
		}
		config.scan.ocr        = parseOcr( scan["ocr"].asString() );
		config.scan.ocr_engine = parseEngine( scan["ocr_engine"].asString() );
		readBool( scan, "japanese_only", config.scan.known_languages_only ); // its earlier name
		readBool( scan, "known_languages_only", config.scan.known_languages_only );
		readBool( scan, "wheel_length", config.scan.wheel_length );
		readBool( scan, "wheel_lock", config.scan.wheel_lock );
		const auto selection  = scan["selection"].asString();
		config.scan.selection = parseSelection( selection );

		const json::Value& popup = root["popup"];
		readInt( popup, "width", config.popup.width, 200, 2000 );
		readInt( popup, "max_height", config.popup.max_height, 100, 2000 );
		readInt( popup, "font_size", config.popup.font_size, 6, 72 );
		readString( popup, "font_family", config.popup.font_family );
		readInt( popup, "offset_x", config.popup.offset_x, -500, 500 );
		readInt( popup, "offset_y", config.popup.offset_y, -500, 500 );
		readInt( popup, "max_results", config.popup.max_results, 1, 200 );
		readInt( popup, "max_dictionaries", config.popup.max_dictionaries, 0, 64 );
		readBool( popup, "show_frequencies", config.popup.show_frequencies );
		readBool( popup, "show_pitch", config.popup.show_pitch );
		readBool( popup, "show_tags", config.popup.show_tags );
		readBool( popup, "show_furigana", config.popup.show_furigana );
		readString( popup, "highlight_color", config.popup.highlight_color );
		readInt( popup, "highlight_thickness", config.popup.highlight_thickness, 1, 12 );
		config.popup.highlight_style = parseHighlight( popup["highlight_style"].asString() );
		config.popup.compositor      = parseCompositor( popup["compositor"].asString() );
		readBool( popup, "highlight_auto", config.popup.highlight_auto );
		config.popup.select_button = popup["select_button"].asString() == "middle" ? MouseButton::Middle : MouseButton::Right;
		config.popup.scale         = std::clamp( popup["scale"].asDouble( 0.0 ), 0.0, 4.0 );
		const auto theme           = popup["theme"].asString();
		config.popup.theme         = parseTheme( theme );
		readInt( popup, "highlight_radius", config.popup.highlight_radius, 0, 32 );
		// One room for both directions, from before they could be told apart; each may still be given its own.
		readInt( popup, "highlight_padding", config.popup.highlight_padding_x, -16, 16 );
		config.popup.highlight_padding_y = config.popup.highlight_padding_x;
		readInt( popup, "highlight_padding_x", config.popup.highlight_padding_x, -16, 16 );
		readInt( popup, "highlight_padding_y", config.popup.highlight_padding_y, -16, 16 );
		config.popup.design    = design_names.parse( popup["design"].asString(), PopupDesign::Friendly );
		config.popup.scheme    = scheme_names.parse( popup["scheme"].asString(), ColorScheme::Default );
		config.popup.placement = placement_names.parse( popup["placement"].asString(), PopupPlacement::BelowText );
		readColor( popup, "background_color", config.popup.background_color );
		readColor( popup, "text_color", config.popup.text_color );
		readColor( popup, "accent_color", config.popup.accent_color );
		readColor( popup, "border_color", config.popup.border_color );
		readInt( popup, "corner_radius", config.popup.corner_radius, 0, 32 );
		readInt( popup, "border_width", config.popup.border_width, 0, 8 );
		readInt( popup, "padding", config.popup.padding, 0, 40 );
		readInt( popup, "opacity", config.popup.opacity, 30, 100 );
		readInt( popup, "headword_size", config.popup.headword_size, 0, 120 );
		readInt( popup, "furigana_size", config.popup.furigana_size, 0, 72 );
		readBool( popup, "show_reading", config.popup.show_reading );
		readBool( popup, "show_inflection", config.popup.show_inflection );
		readBool( popup, "show_dictionary", config.popup.show_dictionary );
		readBool( popup, "show_buttons", config.popup.show_buttons );
		readBool( popup, "show_kanji", config.popup.show_kanji );
		readInt( popup, "max_senses", config.popup.max_senses, 0, 50 );
		readInt( popup, "button_size", config.popup.button_size, 0, 96 );
		for ( const json::Member& color : popup["colors"].members() )
		{
			std::string value;
			readColor( popup["colors"], color.key, value );
			if ( !value.empty() && color.key.size() <= 64 && config.popup.colors.size() < 64 )
			{
				config.popup.colors.push_back( { .name = std::string( color.key ), .value = std::move( value ) } );
			}
		}

		const json::Value& audio = root["audio"];
		readBool( audio, "enabled", config.audio.enabled );
		readBool( audio, "autoplay", config.audio.autoplay );
		readStrings( audio, "sources", config.audio.sources );

		const json::Value& translation = root["translation"];
		readBool( translation, "enabled", config.translation.enabled );
		readBool( translation, "selections", config.translation.selections );
		readString( translation, "sentence_key", config.translation.sentence_key );
		readStrings( translation, "disabled_languages", config.translation.disabled_languages );
		readString( translation, "model", config.translation.model );
		if ( config.translation.model != "full" )
		{
			config.translation.model = "compact";
		}
		// The languages that use weights of their own; anything but "full" means the compact ones.
		for ( const json::Member& chosen : translation["models"].members() )
		{
			std::string weights;
			readString( translation["models"], chosen.key, weights );
			if ( !chosen.key.empty() && chosen.key.size() <= 16 && config.translation.models.size() < 64 )
			{
				config.translation.models.push_back( { .language = std::string( chosen.key ), .model = weights == "full" ? "full" : "compact" } );
			}
		}
		// A key this platform does not know would never be seen held: the default instead of silently none.
		if ( !config.translation.sentence_key.empty() && !parseKey( config.translation.sentence_key ) )
		{
			config.translation.sentence_key = TranslationSettings{}.sentence_key;
		}

		const json::Value& anki = root["anki"];
		readBool( anki, "enabled", config.anki.enabled );
		readString( anki, "url", config.anki.url );
		readString( anki, "key", config.anki.key );
		readString( anki, "deck", config.anki.deck );
		readString( anki, "model", config.anki.model );
		readStrings( anki, "tags", config.anki.tags );
		readBool( anki, "allow_duplicates", config.anki.allow_duplicates );
		for ( const json::Member& field : anki["fields"].members() )
		{
			if ( field.value.isString() )
			{
				config.anki.fields.push_back( { .name = std::string( field.key ), .value = std::string( field.value.asString() ) } );
			}
		}

		for ( const json::Value& item : root["dictionaries"].items() )
		{
			DictionaryPreference preference;
			readString( item, "title", preference.title );
			readBool( item, "enabled", preference.enabled );
			if ( !preference.title.empty() && config.dictionary( preference.title ) == nullptr )
			{
				config.dictionaries.push_back( std::move( preference ) );
			}
		}

		if ( const auto chord = parseChord( config.scan.trigger ); !chord )
		{
			return failWith( "scan.trigger", chord.error() );
		}
		return config;
	}

	Result<Config> Config::parse( std::string_view text )
	{
		const auto document = json::Document::parse( std::string( text ) );
		if ( !document )
		{
			return std::unexpected( document.error() );
		}
		return fromJson( document->root() );
	}

	Result<Config> Config::load( const std::filesystem::path& path )
	{
		const std::ifstream in( path, std::ios::binary );
		if ( !in )
		{
			return Config{};
		}
		std::stringstream buffer;
		buffer << in.rdbuf();
		auto config = parse( buffer.str() );
		if ( !config )
		{
			return failWith( path.string(), config.error() );
		}
		return config;
	}

	Result<> Config::save( const std::filesystem::path& path ) const
	{
		std::error_code ec;
		std::filesystem::create_directories( path.parent_path(), ec );

		auto temp = path;
		temp += ".tmp";
		{
			std::ofstream out( temp, std::ios::binary | std::ios::trunc );
			out << toJson( true ) << '\n';
			if ( !out )
			{
				return fail( "cannot write {}", temp.string() );
			}
		}
		std::filesystem::rename( temp, path, ec );
		if ( ec )
		{
			return fail( "cannot replace {}: {}", path.string(), ec.message() );
		}
		return {};
	}

	std::string Config::toJson( bool pretty ) const
	{
		json::Writer out( pretty );
		out.beginObject();
		out.field( "language", language );
		writeStrings( out, "disabled_languages", disabled_languages );
		out.field( "log_level", log_level );
		out.field( "paused", paused );
		out.field( "statistics", statistics );
		out.field( "statistics_translations", statistics_translations );

		out.key( "scan" ).beginObject();
		writeStrings( out, "trigger", scan.trigger );
		out.field( "max_length", scan.max_length );
		out.field( "delay_ms", scan.delay_ms );
		out.field( "move_threshold", scan.move_threshold );
		out.field( "hide_on_no_result", scan.hide_on_no_result );
		out.field( "search_kanji", scan.search_kanji );
		out.field( "highlight", scan.highlight );
		out.field( "accessibility", scan.accessibility );
		out.field( "selection", selectionName( scan.selection ) );
		writeStrings( out, "ignored_windows", scan.ignored_windows );
		out.field( "ocr", ocrName( scan.ocr ) );
		out.field( "ocr_vertical", scan.ocr_vertical );
		out.field( "ocr_model", scan.ocr_model );
		out.field( "ocr_engine", engineName( scan.ocr_engine ) );
		out.field( "known_languages_only", scan.known_languages_only );
		out.field( "wheel_length", scan.wheel_length );
		out.field( "wheel_lock", scan.wheel_lock );
		writeStrings( out, "ocr_windows", scan.ocr_windows );
		out.endObject();

		out.key( "popup" ).beginObject();
		out.field( "width", popup.width );
		out.field( "max_height", popup.max_height );
		out.field( "font_size", popup.font_size );
		out.field( "font_family", popup.font_family );
		out.field( "scale", popup.scale );
		out.field( "theme", themeName( popup.theme ) );
		out.field( "offset_x", popup.offset_x );
		out.field( "offset_y", popup.offset_y );
		out.field( "max_results", popup.max_results );
		out.field( "max_dictionaries", popup.max_dictionaries );
		out.field( "show_frequencies", popup.show_frequencies );
		out.field( "show_pitch", popup.show_pitch );
		out.field( "show_tags", popup.show_tags );
		out.field( "show_furigana", popup.show_furigana );
		out.field( "highlight_color", popup.highlight_color );
		out.field( "highlight_style", highlightName( popup.highlight_style ) );
		out.field( "compositor", compositorName( popup.compositor ) );
		out.field( "highlight_thickness", popup.highlight_thickness );
		out.field( "highlight_auto", popup.highlight_auto );
		out.field( "select_button", popup.select_button == MouseButton::Middle ? "middle" : "right" );
		out.field( "highlight_radius", popup.highlight_radius );
		out.field( "highlight_padding_x", popup.highlight_padding_x );
		out.field( "highlight_padding_y", popup.highlight_padding_y );
		out.field( "design", design_names.name( popup.design ) );
		out.field( "scheme", scheme_names.name( popup.scheme ) );
		out.field( "placement", placement_names.name( popup.placement ) );
		out.field( "background_color", popup.background_color );
		out.field( "text_color", popup.text_color );
		out.field( "accent_color", popup.accent_color );
		out.field( "border_color", popup.border_color );
		out.field( "corner_radius", popup.corner_radius );
		out.field( "border_width", popup.border_width );
		out.field( "padding", popup.padding );
		out.field( "opacity", popup.opacity );
		out.field( "headword_size", popup.headword_size );
		out.field( "furigana_size", popup.furigana_size );
		out.field( "show_reading", popup.show_reading );
		out.field( "show_inflection", popup.show_inflection );
		out.field( "show_dictionary", popup.show_dictionary );
		out.field( "show_buttons", popup.show_buttons );
		out.field( "show_kanji", popup.show_kanji );
		out.field( "max_senses", popup.max_senses );
		out.field( "button_size", popup.button_size );
		out.key( "colors" ).beginObject();
		for ( const auto& color : popup.colors )
		{
			out.field( color.name, color.value );
		}
		out.endObject();
		out.endObject();

		out.key( "audio" ).beginObject();
		out.field( "enabled", audio.enabled );
		out.field( "autoplay", audio.autoplay );
		writeStrings( out, "sources", audio.sources );
		out.endObject();

		out.key( "translation" ).beginObject();
		out.field( "enabled", translation.enabled );
		out.field( "selections", translation.selections );
		out.field( "sentence_key", translation.sentence_key );
		writeStrings( out, "disabled_languages", translation.disabled_languages );
		out.field( "model", translation.model );
		out.key( "models" ).beginObject();
		for ( const auto& chosen : translation.models )
		{
			out.field( chosen.language, chosen.model );
		}
		out.endObject();
		out.endObject();

		out.key( "anki" ).beginObject();
		out.field( "enabled", anki.enabled );
		out.field( "url", anki.url );
		out.field( "key", anki.key );
		out.field( "deck", anki.deck );
		out.field( "model", anki.model );
		out.key( "fields" ).beginObject();
		for ( const auto& field : anki.fields )
		{
			out.field( field.name, field.value );
		}
		out.endObject();
		writeStrings( out, "tags", anki.tags );
		out.field( "allow_duplicates", anki.allow_duplicates );
		out.endObject();

		out.key( "dictionaries" ).beginArray();
		for ( const auto& dictionary : dictionaries )
		{
			out.beginObject().field( "title", dictionary.title ).field( "enabled", dictionary.enabled ).endObject();
		}
		out.endArray();

		out.endObject();
		return out.take();
	}

	const DictionaryPreference* Config::dictionary( std::string_view title ) const noexcept
	{
		const auto it = std::ranges::find( dictionaries, title, &DictionaryPreference::title );
		return it != dictionaries.end() ? &*it : nullptr;
	}

} // namespace lexiglance::config
