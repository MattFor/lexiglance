#include "Downloader.h"

#include "Common.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Version.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace lexiglance::gui
{

	namespace
	{

		// Dictionaries name their own update locations; anything but https (plain http, file://, ...) is refused.
		bool secure( const QUrl& url )
		{
			return url.scheme() == QStringLiteral( "https" ) && url.isValid();
		}

		QNetworkRequest makeRequest( const QUrl& url )
		{
			QNetworkRequest request( url );
			request.setHeader( QNetworkRequest::UserAgentHeader, QStringLiteral( "Lexiglance/" ) + qs( version ) );
			request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
			// Restarts with every byte that arrives: only a stalled transfer ends, not a slow one.
			request.setTransferTimeout( 30000 );
			return request;
		}

	} // namespace

	Downloader::Downloader( QObject* parent ) :
		QObject( parent ),
		network_( new QNetworkAccessManager( this ) )
	{
	}

	void Downloader::download( const QUrl& url, const QString& target, Progress progress, Done done )
	{
		if ( !secure( url ) )
		{
			done( QStringLiteral( "only https downloads are allowed: " ) + url.toString() );
			return;
		}
		QDir().mkpath( QFileInfo( target ).absolutePath() );
		auto file = std::make_shared<QSaveFile>( target );
		if ( !file->open( QIODevice::WriteOnly ) )
		{
			done( QStringLiteral( "cannot write " ) + target );
			return;
		}

		log::info( "downloading {} to {}", ss( url.toString() ), ss( target ) );
		auto started = std::make_shared<QElapsedTimer>();
		started->start();

		// Qt's transfer timeout also runs while a request queues for a connection (6 per HTTP/1.1 host): time only live transfers.
		QNetworkRequest request = makeRequest( url );
		request.setTransferTimeout( 0 );
		QNetworkReply* reply   = network_->get( request );
		auto           stalled = std::make_shared<bool>( false );
		auto*          idle    = new QTimer( reply );
		idle->setSingleShot( true );
		idle->setInterval( 30000 );
		connect( idle, &QTimer::timeout, reply, [reply, stalled] {
			*stalled = true;
			reply->abort();
		} );
		connect( reply, &QNetworkReply::socketStartedConnecting, idle, qOverload<>( &QTimer::start ) );
		connect( reply, &QNetworkReply::requestSent, idle, qOverload<>( &QTimer::start ) );
		connect( reply, &QNetworkReply::downloadProgress, idle, qOverload<>( &QTimer::start ) );
		// A redirect may queue again on the new host.
		connect( reply, &QNetworkReply::redirected, idle, &QTimer::stop );
		connect( reply, &QNetworkReply::readyRead, this, [reply, file] { file->write( reply->readAll() ); } );
		connect( reply, &QNetworkReply::downloadProgress, this, [progress = std::move( progress )]( qint64 received, qint64 total ) {
			if ( progress )
			{
				progress( received, total );
			}
		} );
		connect( reply, &QNetworkReply::finished, this, [reply, file, target, started, stalled, done = std::move( done )] {
			reply->deleteLater();
			file->write( reply->readAll() );
			const auto size = file->size();
			if ( reply->error() != QNetworkReply::NoError )
			{
				file->cancelWriting();
				const QString error = *stalled ? QStringLiteral( "no data for 30 s" ) : reply->errorString();
				log::warn( "download of {} failed after {} ms: {}", ss( reply->url().toString() ), started->elapsed(), ss( error ) );
				done( error );
				return;
			}
			const bool saved = file->commit();
			if ( saved )
			{
				log::info( "downloaded {} ({}) in {} ms", ss( QFileInfo( target ).fileName() ), ss( formatBytes( static_cast<std::uint64_t>( std::max<qint64>( 0, size ) ) ) ), started->elapsed() );
			}
			else
			{
				log::error( "cannot save {}", ss( target ) );
			}
			done( saved ? QString() : QStringLiteral( "cannot save the download" ) );
		} );
	}

	void Downloader::fetch( const QUrl& url, Fetched done )
	{
		if ( !secure( url ) )
		{
			done( {}, QStringLiteral( "only https downloads are allowed: " ) + url.toString() );
			return;
		}
		QNetworkReply* reply = network_->get( makeRequest( url ) );
		// The whole body stays in memory, and dictionaries name their own index URLs: indexes and SHA256SUMS are a few KB.
		constexpr qint64 limit     = 4 * 1024 * 1024;
		auto             too_large = std::make_shared<bool>( false );
		connect( reply, &QNetworkReply::downloadProgress, this, [reply, too_large]( qint64 received, qint64 ) {
			if ( received > limit && !std::exchange( *too_large, true ) )
			{
				reply->abort();
			}
		} );
		connect( reply, &QNetworkReply::finished, this, [reply, too_large, done = std::move( done )] {
			reply->deleteLater();
			if ( *too_large )
			{
				done( {}, QStringLiteral( "the server sent more than 4 MiB for %1" ).arg( reply->url().toString() ) );
				return;
			}
			if ( reply->error() != QNetworkReply::NoError )
			{
				done( {}, reply->errorString() );
				return;
			}
			done( reply->readAll(), {} );
		} );
	}

	void Downloader::resolve( const QUrl& url, Resolved done )
	{
		if ( !secure( url ) )
		{
			done( {}, QStringLiteral( "only https downloads are allowed: " ) + url.toString() );
			return;
		}
		QNetworkReply* reply = network_->head( makeRequest( url ) );
		connect( reply, &QNetworkReply::finished, this, [reply, done = std::move( done )] {
			reply->deleteLater();
			if ( reply->error() != QNetworkReply::NoError )
			{
				done( {}, reply->errorString() );
				return;
			}
			done( reply->url(), {} );
		} );
	}

} // namespace lexiglance::gui
