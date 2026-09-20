#ifndef LEXIGLANCE_GUI_TRANSLATIONINSTALL_H
#define LEXIGLANCE_GUI_TRANSLATIONINSTALL_H

#include "Downloader.h"

#include <lexiglance/config/Config.h>
#include <lexiglance/language/Language.h>
#include <lexiglance/translate/Model.h>

#include <QObject>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lexiglance::gui
{
	class DaemonClient;
}

// The offline translation models: which are downloaded, and downloading them (for the Translation page, the health
// check's fix and the first-run setup).
namespace lexiglance::gui::translation_install
{

	// A language's model in one of its two precisions: what is downloaded, removed or used.
	struct Wanted
	{
		const lang::Language* language  = nullptr;
		translate::Precision  precision = translate::Precision::Compact;
	};

	// Whether `language` has a model and all its files of that precision are there.
	[[nodiscard]] bool installed( const lang::Language& language, translate::Precision precision );

	// The weights `language` is translated with: the choice made for it, else those already downloaded (a model that is
	// here is not doubled by the other kind), else the one every other language uses.
	[[nodiscard]] translate::Precision precisionFor( const lang::Language& language, const config::TranslationSettings& settings );

	// Each of `languages` with the weights the settings give it.
	[[nodiscard]] std::vector<Wanted> wanted( std::span<const lang::Language* const> languages, const config::TranslationSettings& settings );

	// Those of `models` that have a model, each model and precision once (languages may share a model).
	[[nodiscard]] std::vector<Wanted> distinct( std::span<const Wanted> models );

	// Which of its model's precisions is here: `wanted` when both are, none when neither is.
	[[nodiscard]] std::optional<translate::Precision> present( const lang::Language& language, translate::Precision wanted );

	// Those of `languages` that have a model, each model once (languages may share one).
	[[nodiscard]] std::vector<const lang::Language*> withModel( std::span<const lang::Language* const> languages );

	// The files to download for `models`: those missing, or with `again` all of them.
	[[nodiscard]] std::vector<std::pair<QUrl, QString>> files( std::span<const Wanted> models, bool again = false );

	// How much that is, in bytes, as the language files give it (ONNX Runtime not counted).
	[[nodiscard]] std::uint64_t bytes( std::span<const Wanted> models, bool again = false );

	// The size of one model in that precision.
	[[nodiscard]] std::uint64_t size( const lang::TranslationModel& model, translate::Precision precision );

	// "120 MB".
	[[nodiscard]] QString megabytes( std::uint64_t bytes );

	// Where the model of `language` is kept.
	[[nodiscard]] QString folder( const lang::Language& language );

	// Deletes the model of `language` (and so of every language sharing it), or with `precision` only those weights,
	// leaving the other precision able to translate; an error message, or empty.
	[[nodiscard]] QString remove( const lang::Language& language, std::optional<translate::Precision> precision = std::nullopt );

	// Downloads models (and ONNX Runtime, unless OCR brought it already, and on Windows Microsoft's runtime it needs),
	// then has the daemon use them; a precision already here stays, so either can be chosen. One download at a time.
	class Installer : public QObject
	{
	public:
		using Progress = std::function<void( qint64 received, qint64 total )>;
		// An error message, or empty.
		using Done = std::function<void( const QString& error )>;

		// `owner` asks nothing, but owns Microsoft's installer while it runs (Windows).
		Installer( DaemonClient* client, QWidget* owner );

		void install( std::vector<Wanted> models, bool again, const Progress& progress, Done done );

		[[nodiscard]] bool busy() const noexcept
		{
			return busy_;
		}

	private:
		void finish( const QString& error );

		DaemonClient* client_;
		QWidget*      owner_;
		Downloader*   downloader_;
		bool          busy_ = false;
		Done          done_;
	};

} // namespace lexiglance::gui::translation_install

#endif // LEXIGLANCE_GUI_TRANSLATIONINSTALL_H
