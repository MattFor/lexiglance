#ifndef LEXIGLANCE_GUI_SEARCHPAGE_H
#define LEXIGLANCE_GUI_SEARCHPAGE_H

#include "Common.h"

#include <lexiglance/core/Json.h>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QTextBrowser>
#include <QTimer>

namespace lexiglance::gui
{

	class SearchPage : public Page
	{
	public:
		explicit SearchPage( Context context, QWidget* parent = nullptr );

		void activated() override;

		void setQuery( const QString& text );

	private:
		void search();
		void show( const json::Value& result );

		QLineEdit*    query_;
		QTextBrowser* results_;
		QLabel*       timing_;
		QCheckBox*    watch_;
		QTimer*       debounce_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_SEARCHPAGE_H
