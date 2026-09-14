#ifndef LEXIGLANCE_GUI_OCRGROUP_H
#define LEXIGLANCE_GUI_OCRGROUP_H

#include "Common.h"
#include "Downloader.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/language/Language.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QString>
#include <QUrl>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lexiglance::gui
{

	// Settings for reading text from pixels (games, images, video): mode, engine, and the models of every language.
	class OcrGroup : public QGroupBox
	{
	public:
		explicit OcrGroup( Context context, QWidget* parent = nullptr );

		void refresh();

	private:
		void store();
		void updateStatus();
		// The daemon's status (its answer to "status", or a status.changed event): what capture reads.
		void showStatus( const json::Value& status );
		void updateDownloadButton();
		// The languages OCR models are for: those the daemon reads, else those turned on.
		[[nodiscard]] std::vector<const lang::Language*> ocrLanguages() const;
		// Downloads every file (`what` they are, for the status), then runs `finish` (an error message, or empty) and has
		// the daemon reload its capture.
		void fetchAll( const QString& what, std::vector<std::pair<QUrl, QString>> files, std::function<QString()> finish );
		void downloadTesseract();
		void downloadPaddle();

		Context         context_;
		Downloader*     downloader_;
		QComboBox*      mode_;
		QComboBox*      engine_;
		QCheckBox*      vertical_;
		QComboBox*      model_;
		QPlainTextEdit* windows_;
		QLabel*         status_;
		QPushButton*    download_;
		QProgressBar*   progress_;
		bool            loading_ = false;
		// Codes of the languages the daemon reads (turned on, with dictionaries).
		std::vector<std::string> in_use_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_OCRGROUP_H
