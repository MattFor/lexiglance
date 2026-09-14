#ifndef LEXIGLANCE_DICTIONARY_STRUCTUREDCONTENT_H
#define LEXIGLANCE_DICTIONARY_STRUCTUREDCONTENT_H

#include <lexiglance/dictionary/Format.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Rendering of Yomitan glossary items (plain text, structured content, images) into Pango markup for the popup,
// HTML for the settings application, or plain text.
namespace lexiglance::dict
{

	enum class Markup : std::uint8_t
	{
		Plain,
		Pango,
		Html
	};

	struct TextStyle
	{
		std::string foreground;
		std::string background;
		float       scale     = 1.0F;
		bool        bold      = false;
		bool        italic    = false;
		bool        underline = false;
		bool        strike    = false;
		bool        pill      = false;
		std::int8_t valign    = 0;

		[[nodiscard]] bool plain() const noexcept;
	};

	// The subset of CSS dictionaries ship in styles.css that maps onto inline text: rules whose selector is a single
	// [data-sc-content="..."] or [data-sc-class="..."] attribute, optionally prefixed with an element name.
	class StyleSheet
	{
	public:
		[[nodiscard]] static StyleSheet parse( std::string_view css );

		void apply( std::string_view tag, std::string_view data_content, std::string_view data_class, TextStyle& style ) const;

		[[nodiscard]] bool empty() const noexcept
		{
			return rules_.empty();
		}

	private:
		struct Rule
		{
			std::string   tag;
			std::string   attribute;
			std::string   value;
			TextStyle     style;
			std::uint16_t set = 0;
		};

		std::vector<Rule> rules_;
	};

	struct MarkupOptions
	{
		Markup            format           = Markup::Pango;
		const StyleSheet* styles           = nullptr;
		std::string_view  pill_foreground  = "#ffffff";
		std::string_view  pill_background  = "#565656";
		bool              skip_attribution = true;
	};

	void escapeMarkup( std::string& out, std::string_view text, Markup format );

	void renderGlossary( std::string& out, format::GlossKind kind, std::string_view data, const MarkupOptions& options );

} // namespace lexiglance::dict

#endif // LEXIGLANCE_DICTIONARY_STRUCTUREDCONTENT_H
