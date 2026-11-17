#ifndef LEXIGLANCE_GUI_SEARCHPAGE_H
#define LEXIGLANCE_GUI_SEARCHPAGE_H

#include "Common.h"

#include <lexiglance/core/Json.h>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QTextBrowser>
#include <QTimer>

#include <vector>

namespace lexiglance::gui
{

	class SearchPage : public Page
	{
	public:
		explicit SearchPage( Context context, QWidget* parent = nullptr );

		void activated() override;
		void refresh() override;

		void setQuery( const QString& text );

		// The words of the latest lookups on screen, newest first: listed while nothing is typed.
		void setRecent( QStringList words );

	protected:
		bool eventFilter( QObject* watched, QEvent* event ) override;

	private:
		struct Term
		{
			QString expression;
			QString reading;
		};

		void search();
		void show( const json::Value& result, const QString& text );
		void showRecent();
		// A link under an entry: play, copy or add to Anki.
		void act( const QUrl& url );

		QLineEdit*        query_;
		QTextBrowser*     results_;
		QLabel*           timing_;
		QCheckBox*        watch_;
		QTimer*           debounce_;
		QStringList       recent_;
		std::vector<Term> terms_;
		// The text the entries on show were found for.
		QString shown_query_;
		// What this page copied, which watching the clipboard must not look up.
		QString copied_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_SEARCHPAGE_H
