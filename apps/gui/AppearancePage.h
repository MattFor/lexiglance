#ifndef LEXIGLANCE_GUI_APPEARANCEPAGE_H
#define LEXIGLANCE_GUI_APPEARANCEPAGE_H

#include "Common.h"
#include "Themes.h"

#include <lexiglance/config/Config.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

#include <array>
#include <string>

namespace lexiglance::gui
{

	class AppearancePage : public Page
	{
	public:
		explicit AppearancePage( Context context, QWidget* parent = nullptr );

		void refresh() override;
		void activated() override;

	private:
		// A colour that replaces the scheme's: its button, the button that clears it, and the setting.
		struct ColourSetting
		{
			QPushButton* button                        = nullptr;
			QToolButton* reset                         = nullptr;
			std::string config::PopupSettings::* field = nullptr;
			QString                              name;
		};

		void changed();
		void updatePreview();
		void updateColorButtons();
		void fillThemeMenu( QMenu* menu );
		void fillHighlightMenu( QMenu* menu );
		void applyTheme( const themes::Theme& theme );
		void saveTheme();

		QToolButton*                 themes_;
		QPushButton*                 save_theme_;
		QComboBox*                   design_;
		QComboBox*                   scheme_;
		QComboBox*                   theme_;
		std::array<ColourSetting, 4> colours_;
		QCheckBox*                   default_font_;
		QFontComboBox*               font_;
		QSpinBox*                    font_size_;
		QSpinBox*                    headword_size_;
		QSpinBox*                    furigana_size_;
		QDoubleSpinBox*              scale_;
		QSpinBox*                    width_;
		QSpinBox*                    height_;
		QSpinBox*                    corner_radius_;
		QSpinBox*                    border_width_;
		QSpinBox*                    padding_;
		QSpinBox*                    opacity_;
		QComboBox*                   placement_;
		QSpinBox*                    offset_x_;
		QSpinBox*                    offset_y_;
		QComboBox*                   select_button_;
		QCheckBox*                   furigana_;
		QCheckBox*                   reading_;
		QCheckBox*                   inflection_;
		QCheckBox*                   tags_;
		QCheckBox*                   frequencies_;
		QCheckBox*                   pitch_;
		QCheckBox*                   dictionary_;
		QCheckBox*                   buttons_;
		QSpinBox*                    button_size_;
		QCheckBox*                   kanji_;
		QSpinBox*                    max_senses_;
		QSpinBox*                    results_;
		QSpinBox*                    dictionaries_;
		QToolButton*                 highlight_presets_;
		QComboBox*                   highlight_style_;
		QPushButton*                 highlight_;
		QCheckBox*                   highlight_auto_;
		QSpinBox*                    highlight_thickness_;
		QSpinBox*                    highlight_radius_;
		QSpinBox*                    highlight_padding_x_;
		QSpinBox*                    highlight_padding_y_;
		QComboBox*                   compositor_;
		QLineEdit*                   preview_text_;
		QLabel*                      highlight_preview_;
		QLabel*                      preview_;
		QScrollArea*                 preview_scroll_;
		QTimer*                      preview_timer_;
		bool                         loading_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_APPEARANCEPAGE_H
