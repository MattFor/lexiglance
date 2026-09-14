#ifndef LEXIGLANCE_GUI_DOWNLOADER_H
#define LEXIGLANCE_GUI_DOWNLOADER_H

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

namespace lexiglance::gui
{

	class Downloader : public QObject
	{
	public:
		using Progress = std::function<void( qint64 received, qint64 total )>;
		using Done     = std::function<void( const QString& error )>;
		using Fetched  = std::function<void( const QByteArray& data, const QString& error )>;
		using Resolved = std::function<void( const QUrl& url, const QString& error )>;

		explicit Downloader( QObject* parent = nullptr );

		// Streams `url` into `target` (written atomically).
		void download( const QUrl& url, const QString& target, Progress progress, Done done );

		// Small in-memory request, e.g. a dictionary's update index.
		void fetch( const QUrl& url, Fetched done );

		// Where `url` leads after its redirects (a HEAD request), e.g. a project's latest release.
		void resolve( const QUrl& url, Resolved done );

	private:
		QNetworkAccessManager* network_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_DOWNLOADER_H
