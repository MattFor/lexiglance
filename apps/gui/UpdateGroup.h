#ifndef LEXIGLANCE_GUI_UPDATEGROUP_H
#define LEXIGLANCE_GUI_UPDATEGROUP_H

#include "Downloader.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QString>

#include <functional>

namespace lexiglance::gui
{

	// Keeps Lexiglance up to date from its GitHub releases: finds the newest one and installs the build of it made for
	// this copy (the Windows setup, the portable .zip, the AppImage or the .deb) in its place, checked against the
	// release's SHA256SUMS, then opens it again. Copies that cannot replace themselves are told where the newest is.
	class UpdateGroup : public QGroupBox
	{
	public:
		// How this copy was installed, which decides what an update downloads and how it goes in place.
		enum class Kind;

		explicit UpdateGroup( QWidget* parent = nullptr );

	private:
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

		Downloader*   downloader_;
		QLabel*       status_;
		QLabel*       note_;
		QPushButton*  update_;
		QCheckBox*    automatic_;
		QProgressBar* progress_;
		// The newest release's version, and when it was found, once known.
		QString latest_;
		QString checked_;
		bool    busy_ = false;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_UPDATEGROUP_H
