#ifndef LEXIGLANCE_GUI_OCRINSTALL_H
#define LEXIGLANCE_GUI_OCRINSTALL_H

#include <lexiglance/language/Language.h>

#include <QString>
#include <QUrl>
#include <QWidget>

#include <span>
#include <utility>
#include <vector>

namespace lexiglance::gui::ocr_install
{

	// PaddleOCR model files for the languages (detector, default recogniser when needed, per-language recognisers).
	[[nodiscard]] std::vector<std::pair<QUrl, QString>> paddleFiles( std::span<const lang::Language* const> languages );

	[[nodiscard]] bool paddleInstalled( std::span<const lang::Language* const> languages );

	// Drops files that are already on disk, or returns the full list when nothing is missing (re-download).
	[[nodiscard]] std::vector<std::pair<QUrl, QString>> toDownload( std::vector<std::pair<QUrl, QString>> files );

	// Drops files that are already on disk. An empty result means nothing to fetch.
	[[nodiscard]] std::vector<std::pair<QUrl, QString>> missingOnly( std::vector<std::pair<QUrl, QString>> files );

	// ONNX Runtime release archive name for this machine (without extension), or empty.
	[[nodiscard]] QString runtimeArchive();

	[[nodiscard]] QString runtimeLibraryName();

	[[nodiscard]] QString ocrPath( const char* name );

	// Unpacks a downloaded ONNX Runtime archive into `directory`; empty on success, otherwise an error message.
	[[nodiscard]] QString unpackRuntime( const QString& directory, const QString& archive );

	// Asks whether to fetch the Visual C++ redistributable; returns where to put it, or empty.
	[[nodiscard]] QString askVcRedist( QWidget* parent );

	// Adds ONNX Runtime (and optionally the VC++ redistributable path via `redist_out`) to `files` for a PaddleOCR install.
	// When `prompt_redist` is false, a missing redistributable is included without asking (first-run setup).
	// When `force` is true, the runtime archive is queued even if the library is already on disk (re-download / unpack).
	void appendRuntime( std::vector<std::pair<QUrl, QString>>& files, QString* redist_out, QWidget* ask_parent, bool prompt_redist = true, bool force = false );

	// Where the ONNX Runtime release archive is written (empty when this machine has no matching build).
	[[nodiscard]] QString runtimePackedPath();

} // namespace lexiglance::gui::ocr_install

#endif // LEXIGLANCE_GUI_OCRINSTALL_H
