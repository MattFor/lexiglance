#ifndef LEXIGLANCE_GUI_REPORTOVERLAY_H
#define LEXIGLANCE_GUI_REPORTOVERLAY_H

#include "CardOverlay.h"

#include <lexiglance/core/Json.h>
#include <lexiglance/core/Report.h>

#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lexiglance::gui
{

	class DaemonClient;

	// What a problem report is made of: the daemon's answers, a picture of the latest popup, and what the settings
	// application adds (the Developer page's trace).
	struct Gathered
	{
		std::optional<json::Document> status;
		std::optional<json::Document> health;
		std::optional<json::Document> dictionaries;
		// PNG; empty when no popup was shown yet.
		std::string               picture;
		std::vector<report::Part> extra;

		[[nodiscard]] report::Input input( const QString& what ) const;
		// With the picture of the popup and the settings application's own parts (`extra`) only when asked for.
		[[nodiscard]] std::vector<report::Part> parts( const QString& what, bool picture, bool extras ) const;
	};

	// Asks the daemon for what a report holds, then calls `done`; a daemon that is not running leaves those parts out.
	void gatherReport( DaemonClient& client, std::vector<report::Part> extra, std::function<void( std::shared_ptr<const Gathered> )> done );

	// Saves a report among the person's downloads (else on the desktop, else at home); the file, or empty and `error` says
	// why.
	[[nodiscard]] QString saveReport( const std::vector<report::Part>& parts, QString* error );

	// Report a problem (Overview -> Health, About, the tray menu): what happened in the person's words, and what the
	// report holds, each part open to look at; then the zip is saved, shown in the file manager, and GitHub's issue form
	// opens filled in with the summary, which goes to the clipboard too. Nothing is sent by Lexiglance itself.
	class ReportOverlay final : public CardOverlay
	{
	public:
		// `extra`: parts the settings application adds when the report is made.
		ReportOverlay( DaemonClient& client, std::function<std::vector<report::Part>()> extra, QWidget* parent );

		void open();

	private:
		void showList();
		void showPart( int index );
		void send();
		// Fits the card to what it shows now.
		void refit();

		DaemonClient*                              client_;
		std::function<std::vector<report::Part>()> extra_;
		std::shared_ptr<const Gathered>            gathered_;
		QPlainTextEdit*                            what_;
		QCheckBox*                                 picture_;
		// The Developer page\'s trace, when it gathered one.
		QCheckBox* extras_;
		QLabel*    contents_;
		// The form, or one part of the report looked at in its place.
		QWidget*        form_;
		QWidget*        viewer_;
		QLabel*         part_title_;
		QPlainTextEdit* part_text_;
		QLabel*         part_image_;
		QScrollArea*    part_image_scroll_;
		QLabel*         status_;
		QPushButton*    send_;
		QPushButton*    cancel_;
		QString         saved_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_REPORTOVERLAY_H
