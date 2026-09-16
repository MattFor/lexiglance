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

#include <algorithm>
#include <memory>

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

		QNetworkReply* reply = network_->get( makeRequest( url ) );
		connect( reply, &QNetworkReply::readyRead, this, [reply, file] { file->write( reply->readAll() ); } );
		connect( reply, &QNetworkReply::downloadProgress, this, [progress = std::move( progress )]( qint64 received, qint64 total ) {
			if ( progress )
			{
				progress( received, total );
			}
		} );
		connect( reply, &QNetworkReply::finished, this, [reply, file, target, started, done = std::move( done )] {
			reply->deleteLater();
			file->write( reply->readAll() );
			const auto size = file->size();
			if ( reply->error() != QNetworkReply::NoError )
			{
				file->cancelWriting();
				log::warn( "download of {} failed after {} ms: {}", ss( reply->url().toString() ), started->elapsed(), ss( reply->errorString() ) );
				done( reply->errorString() );
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
		connect( reply, &QNetworkReply::finished, this, [reply, done = std::move( done )] {
			reply->deleteLater();
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
