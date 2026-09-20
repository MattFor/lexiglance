#ifndef LEXIGLANCE_RENDER_POPUPRENDERER_H
#define LEXIGLANCE_RENDER_POPUPRENDERER_H

#include <lexiglance/lookup/Translator.h>
#include <lexiglance/render/Theme.h>

#include <cairo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lexiglance::render
{

	enum class PopupAction : std::uint8_t
	{
		Audio,
		Anki
	};

	// Whether Anki already has a note for an entry (drawn on its add button).
	enum class NoteState : std::uint8_t
	{
		Unknown,
		New,
		Exists
	};

	// The popup's layout: large furigana, plain words and numbered senses (Friendly); Yomitan's look with coloured tags
	// (Classic); or the friendly layout made dense (Compact).
	enum class Design : std::uint8_t
	{
		Friendly,
		Classic,
		Compact
	};

	struct PopupStyle
	{
		Theme       theme      = Theme::dark();
		double      scale      = 1.0;
		int         width      = 400;
		int         max_height = 250;
		double      font_size  = 14.0;
		std::string font_family;
		bool        show_frequencies = true;
		bool        show_pitch       = true;
		bool        show_tags        = true;
		bool        show_furigana    = true;
		// Whether the window can be translucent (a compositor runs): rounded corners and background opacity need it.
		bool rounded      = true;
		bool audio_button = true;
		bool anki_button  = false;
		// The buttons' side in CSS pixels; 0 follows the headword size.
		double button_size = 0.0;

		Design design = Design::Friendly;
		// The window (CSS pixels): inner margin, corner rounding, border; the background's opacity (0 to 1).
		double padding       = 12.0;
		double corner_radius = 10.0;
		double border_width  = 1.0;
		double opacity       = 1.0;
		// Headword and furigana sizes (CSS pixels); 0 follows the design.
		double headword_size   = 0.0;
		double furigana_size   = 0.0;
		bool   show_reading    = false; // full reading beside the headword; independent of furigana
		bool   show_inflection = true;
		bool   show_dictionary = true;
		bool   show_kanji      = true;
		// Definitions shown per dictionary of an entry; 0 shows all.
		int max_senses = 0;

		[[nodiscard]] int px( double css ) const noexcept;

		[[nodiscard]] bool operator==( const PopupStyle& ) const = default;
	};

	// Popup content rendered into an image (device pixels, transparent background). Produced on the render thread and
	// composited into the window by the UI thread, so the UI thread never shapes text.
	class PopupImage
	{
	public:
		// Vertical extent of one entry (content coordinates) and the text a click on it copies. Blocks that are not entries
		// (a translation, a selection no entry covers) have regions too, for copying.
		struct Region
		{
			int         top    = 0;
			int         bottom = 0;
			std::string text;
			bool        entry = true;
		};

		// A clickable icon in the header of an entry (content coordinates).
		struct Button
		{
			PopupAction action = PopupAction::Audio;
			std::size_t entry  = 0;
			int         x      = 0;
			int         y      = 0;
			int         width  = 0;
			int         height = 0;
		};

		// A selectable character (content coordinates) and its bytes in the popup's text.
		struct Glyph
		{
			float         x      = 0.0F;
			float         y      = 0.0F;
			float         width  = 0.0F;
			float         height = 0.0F;
			std::uint32_t begin  = 0;
			std::uint32_t end    = 0;
		};

		PopupImage( cairo_surface_t* surface, int width, int height, std::vector<Region> regions, std::vector<Button> buttons = {}, std::string text = {}, std::vector<Glyph> glyphs = {} ) noexcept :
			surface_( surface ),
			width_( width ),
			height_( height ),
			regions_( std::move( regions ) ),
			buttons_( std::move( buttons ) ),
			text_( std::move( text ) ),
			glyphs_( std::move( glyphs ) )
		{
		}
		~PopupImage();

		PopupImage( const PopupImage& )            = delete;
		PopupImage& operator=( const PopupImage& ) = delete;
		PopupImage( PopupImage&& )                 = delete;
		PopupImage& operator=( PopupImage&& )      = delete;

		[[nodiscard]] cairo_surface_t* surface() const noexcept
		{
			return surface_;
		}

		[[nodiscard]] int width() const noexcept
		{
			return width_;
		}

		[[nodiscard]] int height() const noexcept
		{
			return height_;
		}

		[[nodiscard]] const Region* regionAt( int y ) const noexcept
		{
			for ( const Region& region : regions_ )
			{
				if ( y >= region.top && y < region.bottom )
				{
					return &region;
				}
			}
			return nullptr;
		}

		// Which entry a point belongs to, for an action aimed at the popup rather than at a button (the middle button
		// plays the pronunciation wherever it is pressed). One region is one entry, in the order they are shown.
		[[nodiscard]] std::optional<std::size_t> entryAt( int y ) const noexcept
		{
			std::size_t index = 0;
			for ( const Region& region : regions_ )
			{
				if ( !region.entry )
				{
					continue;
				}
				if ( y >= region.top && y < region.bottom )
				{
					return index;
				}
				++index;
			}
			return std::nullopt;
		}

		[[nodiscard]] const Button* buttonAt( int x, int y ) const noexcept
		{
			for ( const Button& button : buttons_ )
			{
				if ( x >= button.x && x < button.x + button.width && y >= button.y && y < button.y + button.height )
				{
					return &button;
				}
			}
			return nullptr;
		}

		[[nodiscard]] const std::vector<Glyph>& glyphs() const noexcept
		{
			return glyphs_;
		}

		// The glyph at or nearest to a point (content coordinates); none when the popup has no text.
		[[nodiscard]] std::optional<std::size_t> glyphAt( int x, int y ) const noexcept;

		// Glyphs first..last (either order) as text, with line breaks between lines and spaces between separate items.
		[[nodiscard]] std::string selectedText( std::size_t first, std::size_t last ) const;

	private:
		cairo_surface_t*    surface_;
		int                 width_;
		int                 height_;
		std::vector<Region> regions_;
		std::vector<Button> buttons_;
		std::string         text_;
		std::vector<Glyph>  glyphs_;
	};

	// A translation shown above the entries: the text translated, and its translation once it is there (empty: still on
	// its way) or why there is none.
	struct Translation
	{
		std::string source;
		std::string text;
		std::string problem;
	};

	// Not thread-safe: each thread that renders owns its renderer (and with it its own Pango font map).
	class PopupRenderer
	{
	public:
		PopupRenderer();
		~PopupRenderer();

		PopupRenderer( const PopupRenderer& )            = delete;
		PopupRenderer& operator=( const PopupRenderer& ) = delete;
		PopupRenderer( PopupRenderer&& )                 = delete;
		PopupRenderer& operator=( PopupRenderer&& )      = delete;

		void setStyle( const PopupStyle& style );

		[[nodiscard]] const PopupStyle& style() const noexcept;

		// Returns nullptr for an empty result.
		// `limit` stops laying entries out once the content is that tall (0: all of it), for a quick first image.
		// With a translation, a popup is made even when nothing was found in the dictionaries.
		[[nodiscard]] std::shared_ptr<const PopupImage> render( const lookup::LookupResult& result, std::span<const NoteState> notes = {}, int limit = 0, const Translation* translation = nullptr );

		// Loads fonts ahead of the first popup.
		void warmUp();

		struct Impl;

	private:
		std::unique_ptr<Impl> impl_;
	};

	struct PopupSize
	{
		int width  = 0;
		int height = 0;
	};

	[[nodiscard]] PopupSize popupSize( const PopupImage& content, const PopupStyle& style ) noexcept;

	// Paints frame, the visible slice of the content, a scrollbar and an optional status badge into a window-sized context.
	void composePopup( cairo_t* cr, const PopupImage& content, int scroll, PopupSize size, const PopupStyle& style, std::string_view badge = {} );

	// Shades the selected glyphs first..last over a composed popup.
	void drawSelection( cairo_t* cr, const PopupImage& content, std::size_t first, std::size_t last, int scroll, PopupSize size, const PopupStyle& style );

	// Black text on white, `pixel_size` high, as XRGB pixels row by row (for OCR self-tests).
	struct TextRaster
	{
		int                        width  = 0;
		int                        height = 0;
		std::vector<std::uint32_t> pixels;
	};

	[[nodiscard]] TextRaster rasterizeText( std::string_view text, double pixel_size, std::string_view family = {} );

	// How a font family (empty: the popup's default) with its fallbacks covers `text`: the family the text starts in,
	// and how many characters have no glyph in any installed font.
	struct FontCoverage
	{
		std::string family;
		int         missing = 0;
	};

	[[nodiscard]] FontCoverage checkFont( std::string_view text, std::string_view family = {} );

	// The family to ask Pango for: `families` (comma separated; empty: the popup's default) as the platform's font map
	// takes it. Windows' font map takes a single family only (a list falls back to Arial Unicode MS): the first installed.
	[[nodiscard]] std::string fontFamilies( std::string_view families = {} );

} // namespace lexiglance::render

#endif // LEXIGLANCE_RENDER_POPUPRENDERER_H
