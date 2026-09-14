#include "Downloader.h"

#include "Common.h"

#include <lexiglance/core/Version.h>

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

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

		QNetworkReply* reply = network_->get( makeRequest( url ) );
		connect( reply, &QNetworkReply::readyRead, this, [reply, file] { file->write( reply->readAll() ); } );
		connect( reply, &QNetworkReply::downloadProgress, this, [progress = std::move( progress )]( qint64 received, qint64 total ) {
			if ( progress )
			{
				progress( received, total );
			}
		} );
		connect( reply, &QNetworkReply::finished, this, [reply, file, done = std::move( done )] {
			reply->deleteLater();
			file->write( reply->readAll() );
			if ( reply->error() != QNetworkReply::NoError )
			{
				file->cancelWriting();
				done( reply->errorString() );
				return;
			}
			done( file->commit() ? QString() : QStringLiteral( "cannot save the download" ) );
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

} // namespace lexiglance::gui
