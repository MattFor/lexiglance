#ifndef LEXIGLANCE_GUI_THEMES_H
#define LEXIGLANCE_GUI_THEMES_H

#include <lexiglance/config/Config.h>

#include <QString>

#include <vector>

// Themes: the look of the popup and of the highlight (design, colours, window, sizes), apart from what the popup shows
// and where it opens. Some are built in; any others are JSON files in the themes directory, which anyone can write by
// hand, save from the current look, or share (docs/themes.md).
namespace lexiglance::gui::themes
{

	struct Theme
	{
		QString name;
		QString description;
		// The theme file; empty for a built-in theme.
		QString path;
		// Built in: the theme's settings as JSON.
		QString json;
	};

	[[nodiscard]] QString directory();

	[[nodiscard]] std::vector<Theme> builtIn();

	// The theme files in the themes directory, by name.
	[[nodiscard]] std::vector<Theme> installed();

	// Ready-made highlights: they set the highlight's settings only.
	[[nodiscard]] std::vector<Theme> highlights();

	// Gives `popup` the theme's look. What the theme leaves out takes its default, so a theme always looks the same.
	[[nodiscard]] bool apply( const Theme& theme, config::PopupSettings& popup, QString* error = nullptr );

	// Saves the look of `popup` as a theme file; returns its path, empty on failure.
	[[nodiscard]] QString save( const QString& name, const config::PopupSettings& popup, QString* error = nullptr );

	// Copies a theme file into the themes directory after checking it; returns the copy's path, empty on failure.
	[[nodiscard]] QString import( const QString& file, QString* error = nullptr );

} // namespace lexiglance::gui::themes

#endif // LEXIGLANCE_GUI_THEMES_H
