#include "Themes.h"

#include "Common.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Paths.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <string_view>

namespace lexiglance::gui::themes
{

	namespace
	{

		// The popup settings a theme consists of (the same names as in config.json).
		constexpr std::array<std::string_view, 22> look_keys{
			"design",
			"scheme",
			"background_color",
			"text_color",
			"accent_color",
			"border_color",
			"colors",
			"corner_radius",
			"border_width",
			"padding",
			"opacity",
			"headword_size",
			"furigana_size",
			"highlight_style",
			"highlight_color",
			"highlight_thickness",
			"highlight_radius",
			"highlight_padding",
			"highlight_auto",
			"font_family",
			"show_tags",
			"show_dictionary",
		};

		// Of these, a highlight preset sets only these.
		constexpr std::array<std::string_view, 6> highlight_keys{ "highlight_style", "highlight_color", "highlight_thickness", "highlight_radius", "highlight_padding", "highlight_auto" };

		Theme preset( const char* name, const char* description, const char* json )
		{
			return { .name = QString::fromUtf8( name ), .description = QString::fromUtf8( description ), .path = {}, .json = QString::fromUtf8( json ) };
		}

		// The popup object of a theme: {"popup": {...}} or the settings themselves.
		const json::Value* popupOf( const json::Value& root )
		{
			const auto* popup = root.find( "popup" );
			return popup != nullptr && popup->isObject() ? popup : &root;
		}

		// The look settings of a popup object, as JSON members (only `keys`).
		std::string pick( const json::Value& popup, std::span<const std::string_view> keys )
		{
			json::Writer out;
			out.beginObject();
			for ( const auto key : keys )
			{
				if ( const auto* value = popup.find( key ) )
				{
					out.key( key ).raw( json::serialize( *value ) );
				}
			}
			out.endObject();
			return out.take();
		}

		bool applyKeys( const QString& text, std::span<const std::string_view> keys, config::PopupSettings& popup, QString* error )
		{
			const auto document = json::Document::parse( ss( text ) );
			if ( !document || !document->root().isObject() )
			{
				if ( error != nullptr )
				{
					*error = QStringLiteral( "Not a theme: %1" ).arg( document ? QStringLiteral( "not a JSON object" ) : qs( document.error().message ) );
				}
				return false;
			}
			// The theme's settings over the defaults, read by the configuration's own parser (ranges, colours checked).
			const auto parsed = config::Config::parse( "{\"popup\":" + pick( *popupOf( document->root() ), keys ) + "}" );
			if ( !parsed )
			{
				if ( error != nullptr )
				{
					*error = qs( parsed.error().message );
				}
				return false;
			}
			const auto& theme = parsed->popup;
			const auto  has   = [&]( std::string_view key ) { return std::ranges::contains( keys, key ); };
			if ( has( "design" ) )
			{
				popup.design           = theme.design;
				popup.scheme           = theme.scheme;
				popup.background_color = theme.background_color;
				popup.text_color       = theme.text_color;
				popup.accent_color     = theme.accent_color;
				popup.border_color     = theme.border_color;
				popup.colors           = theme.colors;
				popup.corner_radius    = theme.corner_radius;
				popup.border_width     = theme.border_width;
				popup.padding          = theme.padding;
				popup.opacity          = theme.opacity;
				popup.headword_size    = theme.headword_size;
				popup.furigana_size    = theme.furigana_size;
				popup.font_family      = theme.font_family;
				popup.show_tags        = theme.show_tags;
				popup.show_dictionary  = theme.show_dictionary;
			}
			popup.highlight_style     = theme.highlight_style;
			popup.highlight_color     = theme.highlight_color;
			popup.highlight_thickness = theme.highlight_thickness;
			popup.highlight_radius    = theme.highlight_radius;
			popup.highlight_padding   = theme.highlight_padding;
			popup.highlight_auto      = theme.highlight_auto;
			return true;
		}

		QString readFile( const QString& path, QString* error )
		{
			QFile file( path );
			if ( !file.open( QIODevice::ReadOnly ) || file.size() > qint64{ 256 } * 1024 )
			{
				if ( error != nullptr )
				{
					*error = QStringLiteral( "Cannot read %1" ).arg( path );
				}
				return {};
			}
			return QString::fromUtf8( file.readAll() );
		}

		QString nameOf( const QString& path )
		{
			if ( const auto document = json::Document::parse( ss( readFile( path, nullptr ) ) ); document && document->root()["name"].isString() )
			{
				return qs( document->root()["name"].asString() );
			}
			return QFileInfo( path ).completeBaseName();
		}

		// A file name for a theme name; a number is added while one exists.
		QString freePath( const QString& name )
		{
			QString base = name.trimmed().toLower();
			base.replace( QRegularExpression( QStringLiteral( "[^\\w-]+" ) ), QStringLiteral( "-" ) );
			base.remove( QRegularExpression( QStringLiteral( "^-+|-+$" ) ) );
			if ( base.isEmpty() )
			{
				base = QStringLiteral( "theme" );
			}
			QString path = directory() + "/" + base + ".json";
			for ( int n = 2; QFile::exists( path ); ++n )
			{
				path = directory() + "/" + base + "-" + QString::number( n ) + ".json";
			}
			return path;
		}

	} // namespace

	QString directory()
	{
		return qs( ( paths::configDir() / "themes" ).string() );
	}

	std::vector<Theme> builtIn()
	{
		return {
			preset( "Friendly", "Large furigana, plain words, numbered senses (the default)", R"({"design":"friendly","scheme":"default","corner_radius":10,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                                                                  R"("highlight_style":"fill","highlight_color":"#5aa0ff59","highlight_radius":4,"highlight_padding":2,"highlight_thickness":2})" ),
			preset( "Classic (Yomitan)", "Yomitan's look: coloured tags, small furigana", R"({"design":"classic","scheme":"default","corner_radius":7,"border_width":1,"padding":10,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                                                              R"("highlight_style":"underline","highlight_color":"#5aa0ffff","highlight_radius":0,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Compact", "Everything, closer together", R"({"design":"compact","scheme":"default","corner_radius":6,"border_width":1,"padding":8,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                                  R"("highlight_style":"underline","highlight_color":"#5aa0ffff","highlight_radius":1,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Minimal", "No border, no tags, soft corners", R"({"design":"friendly","scheme":"default","corner_radius":14,"border_width":0,"padding":14,"opacity":96,"show_tags":false,"show_dictionary":false,)"
			                                                       R"("highlight_style":"fill","highlight_color":"#5aa0ff40","highlight_radius":6,"highlight_padding":3,"highlight_thickness":2})" ),
			preset( "Paper", "Warm paper and ink", R"({"design":"friendly","scheme":"paper","corner_radius":8,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                       R"("highlight_style":"fill","highlight_color":"#f0a0505a","highlight_radius":3,"highlight_padding":2,"highlight_thickness":2})" ),
			preset( "Nord", "Cool arctic blues", R"({"design":"friendly","scheme":"nord","corner_radius":10,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                     R"("highlight_style":"outline","highlight_color":"#88c0d0ff","highlight_radius":5,"highlight_padding":2,"highlight_thickness":2})" ),
			preset( "Sakura", "Cherry blossom pink", R"({"design":"friendly","scheme":"sakura","corner_radius":14,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                         R"("highlight_style":"fill","highlight_color":"#f28cb159","highlight_radius":8,"highlight_padding":3,"highlight_thickness":2})" ),
			preset( "Matcha", "Green tea", R"({"design":"friendly","scheme":"matcha","corner_radius":10,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                               R"("highlight_style":"wavy-underline","highlight_color":"#7fb04aff","highlight_radius":0,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Midnight", "Black for OLED screens", R"({"design":"friendly","scheme":"midnight","corner_radius":10,"border_width":1,"padding":12,"opacity":100,"show_tags":true,"show_dictionary":true,)"
			                                              R"("highlight_style":"brackets","highlight_color":"#7aa2ffff","highlight_radius":1,"highlight_padding":3,"highlight_thickness":2})" ),
			preset( "High contrast", "Black and white, thick lines, for low vision", R"({"design":"friendly","scheme":"contrast","corner_radius":4,"border_width":2,"padding":12,"opacity":100,"headword_size":36,"furigana_size":20,"show_tags":true,"show_dictionary":true,)"
			                                                                         R"("highlight_style":"outline","highlight_color":"#ffd400ff","highlight_radius":0,"highlight_padding":2,"highlight_thickness":3})" ),
		};
	}

	std::vector<Theme> highlights()
	{
		return {
			preset( "Soft marker", "A yellow highlighter over the word", R"({"highlight_style":"fill","highlight_color":"#ffd54a66","highlight_radius":3,"highlight_padding":2,"highlight_thickness":2})" ),
			preset( "Automatic marker", "A highlighter in a colour that suits the background", R"({"highlight_style":"fill","highlight_color":"#5aa0ff59","highlight_radius":3,"highlight_padding":2,"highlight_auto":true})" ),
			preset( "Pill", "A rounded capsule", R"({"highlight_style":"fill","highlight_color":"#5aa0ff4d","highlight_radius":32,"highlight_padding":3,"highlight_thickness":2})" ),
			preset( "Underline", "A plain line under the word", R"({"highlight_style":"underline","highlight_color":"#5aa0ffff","highlight_radius":1,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Double underline", "Two thin lines", R"({"highlight_style":"double-underline","highlight_color":"#e0457bff","highlight_radius":0,"highlight_padding":1,"highlight_thickness":1})" ),
			preset( "Dotted underline", "A row of dots", R"({"highlight_style":"dotted-underline","highlight_color":"#5aa0ffff","highlight_radius":1,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Spell-check wave", "A wavy line, like a spelling checker's", R"({"highlight_style":"wavy-underline","highlight_color":"#e0457bff","highlight_radius":0,"highlight_padding":1,"highlight_thickness":2})" ),
			preset( "Rounded outline", "A frame with round corners", R"({"highlight_style":"outline","highlight_color":"#5aa0ffff","highlight_radius":6,"highlight_padding":2,"highlight_thickness":2})" ),
			preset( "Focus brackets", "Corner marks, like a camera's focus frame", R"({"highlight_style":"brackets","highlight_color":"#5aa0ffff","highlight_radius":1,"highlight_padding":3,"highlight_thickness":2})" ),
		};
	}

	std::vector<Theme> installed()
	{
		std::vector<Theme> themes;
		const QDir         dir( directory() );
		for ( const QFileInfo& file : dir.entryInfoList( { QStringLiteral( "*.json" ) }, QDir::Files, QDir::Name ) )
		{
			themes.push_back( { .name = nameOf( file.absoluteFilePath() ), .description = {}, .path = file.absoluteFilePath(), .json = {} } );
		}
		std::ranges::sort( themes, []( const Theme& a, const Theme& b ) { return QString::localeAwareCompare( a.name, b.name ) < 0; } );
		return themes;
	}

	bool apply( const Theme& theme, config::PopupSettings& popup, QString* error )
	{
		const QString text = theme.path.isEmpty() ? theme.json : readFile( theme.path, error );
		if ( text.isEmpty() )
		{
			return false;
		}
		// A preset without "design" is a highlight preset.
		const auto document  = json::Document::parse( ss( text ) );
		const bool highlight = document && document->root().isObject() && popupOf( document->root() )->find( "design" ) == nullptr;
		return highlight ? applyKeys( text, highlight_keys, popup, error ) : applyKeys( text, look_keys, popup, error );
	}

	QString save( const QString& name, const config::PopupSettings& popup, QString* error )
	{
		config::Config config;
		config.popup        = popup;
		const auto document = json::Document::parse( config.toJson( false ) );
		if ( !document || !QDir().mkpath( directory() ) )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot create %1" ).arg( directory() );
			}
			return {};
		}
		json::Writer out( true );
		out.beginObject().field( "name", ss( name.trimmed() ) ).field( "lexiglance_theme", 1 ).key( "popup" ).raw( pick( document->root()["popup"], look_keys ) ).endObject();

		QString   path = freePath( name );
		QSaveFile file( path );
		if ( !file.open( QIODevice::WriteOnly ) || file.write( QByteArray::fromStdString( out.take() + "\n" ) ) < 0 || !file.commit() )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot write %1" ).arg( path );
			}
			return {};
		}
		return path;
	}

	QString import( const QString& file, QString* error )
	{
		const QString text = readFile( file, error );
		if ( text.isEmpty() )
		{
			return {};
		}
		config::PopupSettings probe;
		if ( !apply( { .name = {}, .description = {}, .path = {}, .json = text }, probe, error ) )
		{
			return {};
		}
		const auto    document = json::Document::parse( ss( text ) );
		const QString name     = document && document->root()["name"].isString() ? qs( document->root()["name"].asString() ) : QFileInfo( file ).completeBaseName();
		QDir().mkpath( directory() );
		QString path = freePath( name );
		if ( !QFile::copy( file, path ) )
		{
			if ( error != nullptr )
			{
				*error = QStringLiteral( "Cannot copy the theme to %1" ).arg( path );
			}
			return {};
		}
		return path;
	}

} // namespace lexiglance::gui::themes
