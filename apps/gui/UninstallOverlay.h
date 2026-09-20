#ifndef LEXIGLANCE_GUI_UNINSTALLOVERLAY_H
#define LEXIGLANCE_GUI_UNINSTALLOVERLAY_H

#include "CardOverlay.h"

#include <lexiglance/core/Uninstall.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <vector>

namespace lexiglance::gui
{

	class DaemonClient;

	// Uninstall, on a card over the window: what goes, and whether the settings, dictionaries and downloaded models go
	// too (they do unless unticked). Then the removal itself and how it went; the settings application ends after it.
	// The Windows setup's copy is removed by its own uninstaller, a portable one by a script once this program has ended.
	class UninstallOverlay final : public CardOverlay
	{
	public:
		UninstallOverlay( DaemonClient* client, QWidget* parent );

		void open();

	private:
		void describe();
		void start();
		// Ends the daemon and the other copies of the settings application, then runs `then`.
		void stopPrograms( std::function<void()> then );
		// After the part that takes root: the daemon and the rest of the files.
		void removeRest( const uninstall::Plan& plan, const std::vector<std::filesystem::path>& own );
		void finish( const uninstall::Plan& plan, const std::vector<std::string>& failures );
		// Nothing was removed: why, and the card as before.
		void fail( const QString& message );
		void setBusy( bool busy );
#ifdef Q_OS_WIN
		// A portable copy cannot delete itself while it runs: a script does it once this program has ended.
		void removePortable( const uninstall::Plan& plan );
#endif

		DaemonClient* client_;
		QLabel*       text_;
		QCheckBox*    erase_;
		QLabel*       erase_note_;
		QLabel*       status_;
		QPushButton*  cancel_;
		QPushButton*  uninstall_;
		bool          finished_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_UNINSTALLOVERLAY_H
