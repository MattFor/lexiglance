#ifndef LEXIGLANCE_GUI_UPDATEGROUP_H
#define LEXIGLANCE_GUI_UPDATEGROUP_H

#include "Downloader.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLockFile>
#include <QProgressBar>
#include <QPushButton>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace lexiglance::gui
{

	// Keeps Lexiglance up to date from its GitHub releases: finds the newest one and installs the build of it made for
	// this copy (the Windows setup, the portable .zip, the AppImage, the .deb or the .rpm) in its place, checked against the
	// release's SHA256SUMS, then opens it again. Copies that cannot replace themselves are told where the newest is.
	class UpdateGroup : public QGroupBox
	{
	public:
		// How this copy was installed, which decides what an update downloads and how it goes in place.
		enum class Kind : std::uint8_t
		{
			WindowsSetup,    // installed with lexiglance-windows-setup.exe
			WindowsPortable, // unpacked from lexiglance-windows-portable.zip
			AppImage,        // lexiglance-x86_64.AppImage
			Deb,             // the lexiglance-amd64.deb package
			Rpm,             // the lexiglance-x86_64.rpm package
			Source,          // a build tree
			Other,           // the .tar.gz, or a system or processor there are no releases for
		};

		// `background`: never shown (lexiglance --background-update): installs a newer version if updates are automatic,
		// starts the daemon of the new version, and quits the application when there is nothing (more) to do.
		explicit UpdateGroup( QWidget* parent = nullptr, bool background = false );

	private:
		// The end of a background update, whatever became of it.
		void done() const;
		// On a timer: installs a newer version where that needs no questions, else only looks for one.
		void runAutomatically();
		// Finds the newest release (latest_), then runs `then`.
		void check( std::function<void()> then = {} );
		// Installs the newest release, even the version already running unless `automatic`.
		void update( bool automatic );
		void download( Kind kind );
		void install( Kind kind, const QString& file );
		void showState();
		void fail( const QString& message );
		void setBusy( bool busy );
		// Windows: after an update, fetch the Visual C++ Redistributable when OCR would need it and this machine has
		// none, so people coming from a build that never offered it get it without opening Health.
		void ensureVcRedist();

		Downloader*   downloader_;
		QLabel*       status_;
		QLabel*       note_;
		QPushButton*  update_;
		QCheckBox*    automatic_;
		QProgressBar* progress_;
		// The newest release's version, and when it was found, once known.
		QString latest_;
		QString checked_;
		bool    busy_       = false;
		bool    background_ = false;
		// Held from the download to the installer's start, so two copies never update at once.
		std::unique_ptr<QLockFile> lock_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_UPDATEGROUP_H
