#ifndef LEXIGLANCE_GUI_SCANNINGPAGE_H
#define LEXIGLANCE_GUI_SCANNINGPAGE_H

#include "Common.h"
#include "OcrGroup.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	class ScanningPage : public Page
	{
	public:
		explicit ScanningPage( const Context& context, QWidget* parent = nullptr );

		void refresh() override;

	private:
		void record();
		void storeTrigger();
		// The last language on cannot be turned off.
		void updateLanguageBoxes();

		std::array<QComboBox*, 3> keys_{};
		QPushButton*              record_;
		QLabel*                   record_hint_;
		QSpinBox*                 length_;
		QSpinBox*                 delay_;
		QSpinBox*                 threshold_;
		QCheckBox*                hide_empty_;
		QCheckBox*                kanji_;
		QCheckBox*                highlight_;
		QCheckBox*                accessibility_;
		QCheckBox*                known_languages_;
		// A box for each language, with its code.
		std::vector<std::pair<QCheckBox*, std::string>> language_boxes_;
		QCheckBox*                                      wheel_;
		QCheckBox*                                      wheel_lock_;
		QComboBox*                                      selection_;
		QPlainTextEdit*                                 ignored_;
		OcrGroup*                                       ocr_;
		bool                                            recording_ = false;
		bool                                            loading_   = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_SCANNINGPAGE_H
