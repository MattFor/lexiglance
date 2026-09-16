#ifndef LEXIGLANCE_GUI_VCREDIST_H
#define LEXIGLANCE_GUI_VCREDIST_H

#include <QString>
#include <QUrl>

#include <functional>

class QObject;

// Microsoft's Visual C++ Redistributable, which their build of onnxruntime.dll imports and Lexiglance itself does not.
// Without it OCR cannot read anything, so Lexiglance fetches and installs it rather than sending the user to a website.
// Nothing here does anything off Windows.
namespace lexiglance::gui::vcredist
{

	// The runtime libraries Windows cannot find, as a readable list; empty when it has them all, and always empty off
	// Windows.
	[[nodiscard]] QString missing();

	// Microsoft's installer for this machine.
	[[nodiscard]] QUrl url();

	// Where a download of the installer goes.
	[[nodiscard]] QString installerPath();

	// Runs a downloaded installer and removes it again; `done` gets an error message, empty once Windows has the
	// runtime. The installer asks for administrator rights itself, and never reboots on its own. `owner` keeps the
	// process alive: `done` does not come once it is gone.
	void install( QObject* owner, const QString& installer, const std::function<void( const QString& )>& done );

} // namespace lexiglance::gui::vcredist

#endif // LEXIGLANCE_GUI_VCREDIST_H
