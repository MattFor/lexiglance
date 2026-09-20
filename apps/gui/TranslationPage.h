#ifndef LEXIGLANCE_GUI_TRANSLATIONPAGE_H
#define LEXIGLANCE_GUI_TRANSLATIONPAGE_H

#include "Common.h"
#include "TranslationInstall.h"

#include <lexiglance/language/Language.h>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>

#include <optional>
#include <vector>

namespace lexiglance::gui
{

	// Translating sentences offline: whether and with which key, and one row per language, ticked to be translated and
	// ticked again under the weights it translates with, downloaded, downloaded again or removed as dictionaries are.
	// A sentence can be tried out below.
	class TranslationPage : public Page
	{
	public:
		explicit TranslationPage( Context context, QWidget* parent = nullptr );

		void refresh() override;
		void activated() override;

	protected:
		void hideEvent( QHideEvent* event ) override;
		void changeEvent( QEvent* event ) override;

	private:
		// What removing the selection would delete: the weights a language has but does not translate with, or with no
		// precision its whole model.
		struct Removal
		{
			const lang::Language* language = nullptr;
			// None: everything in the model's folder.
			std::optional<translate::Precision> precision;
		};

		void store();
		// The keys that can be held with the trigger, the current one chosen.
		void                                fillKeys();
		[[nodiscard]] const lang::Language* languageOf( const QTreeWidgetItem* item ) const;
		// A box was ticked: one of the two weights columns turns the other off, since a language translates with one.
		void ticked( QTreeWidgetItem* item, int column );
		// The precision above the table: every language follows it, until one is ticked on its own again.
		void applyPrecisionToAll();
		// Every row as things are now: the ticks, and which weights are downloaded.
		void updateRows();
		void updateButtons();
		void showDetails();
		void download( std::vector<translation_install::Wanted> models, bool again );
		void removeSelected();
		void translate();
		// The selected languages, each with the weights it translates with.
		[[nodiscard]] std::vector<translation_install::Wanted> selected() const;
		// Ticked languages in use (not off in Scanning) whose model is missing in the precision they ask for.
		[[nodiscard]] std::vector<translation_install::Wanted> missing() const;
		// The models and weights the Remove button would delete, each once.
		[[nodiscard]] std::vector<Removal> removable() const;
		// The weights `language` is translated with, as the settings say.
		[[nodiscard]] translate::Precision precisionOf( const lang::Language& language ) const;

		translation_install::Installer* installer_;
		QCheckBox*                      enabled_;
		QCheckBox*                      selections_;
		QComboBox*                      key_;
		QComboBox*                      precision_;
		QTreeWidget*                    table_;
		QTextBrowser*                   details_;
		QPushButton*                    download_;
		QPushButton*                    again_;
		QPushButton*                    remove_;
		QLabel*                         activity_;
		QProgressBar*                   progress_;
		QLineEdit*                      test_text_;
		QComboBox*                      test_language_;
		QPushButton*                    test_button_;
		QLabel*                         test_result_;
		// Models appear and go from elsewhere too (the first-run setup, Health's fix): looked at while the page is shown.
		QTimer* poll_;
		// The languages that have a model, in the rows' order.
		std::vector<const lang::Language*> languages_;
		// Those being downloaded now.
		std::vector<const lang::Language*> downloading_;
		// What the details show, so the poll leaves them (and a selection in them) alone when nothing changed.
		QString details_html_;
		bool    loading_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_TRANSLATIONPAGE_H
