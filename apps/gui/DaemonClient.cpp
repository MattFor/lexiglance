#include "DaemonClient.h"

#include "Common.h"

#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>

#include <QProcess>

namespace lexiglance::gui
{

	DaemonClient::DaemonClient( QObject* parent ) :
		QObject( parent ),
		socket_( new QLocalSocket( this ) ),
		retry_( new QTimer( this ) )
	{
		retry_->setInterval( 1500 );
		connect( retry_, &QTimer::timeout, this, [this] {
			if ( std::chrono::steady_clock::now() > fast_until_ )
			{
				retry_->setInterval( 1500 );
			}
			connectNow();
		} );
		connect( socket_, &QLocalSocket::connected, this, [this] {
			retry_->stop();
			notifyConnection( true );
		} );
		connect( socket_, &QLocalSocket::disconnected, this, [this] {
			failPending( QStringLiteral( "the daemon disconnected" ) );
			notifyConnection( false );
			retry_->start();
		} );
		connect( socket_, &QLocalSocket::errorOccurred, this, [this]( QLocalSocket::LocalSocketError ) {
			if ( socket_->state() == QLocalSocket::UnconnectedState && !retry_->isActive() )
			{
				retry_->start();
			}
		} );
		connect( socket_, &QLocalSocket::readyRead, this, [this] { read(); } );
	}

	void DaemonClient::connectNow()
	{
		if ( socket_->state() == QLocalSocket::UnconnectedState )
		{
			socket_->connectToServer( qs( paths::ipcEndpoint() ) );
		}
	}

	bool DaemonClient::startDaemon( bool replace )
	{
		const bool started = QProcess::startDetached( daemonExecutable(), replace ? QStringList{ QStringLiteral( "--replace" ) } : QStringList{} );
		if ( started )
		{
			log::info( "started {}{}", ss( daemonExecutable() ), replace ? " --replace" : "" );
		}
		else
		{
			log::error( "cannot start {}", ss( daemonExecutable() ) );
		}
		expectDaemon();
		return started;
	}

	void DaemonClient::expectDaemon()
	{
		fast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds( 15 );
		retry_->setInterval( 200 );
		if ( !retry_->isActive() && !connected() )
		{
			retry_->start();
		}
		connectNow();
	}

	void DaemonClient::call( std::string_view method, const std::string& params, Reply reply, int timeout_ms )
	{
		if ( !connected() )
		{
			if ( reply )
			{
				reply( nullptr, QStringLiteral( "Lexiglance is not running" ) );
			}
			return;
		}
		const std::int64_t id = next_id_++;
		if ( reply )
		{
			pending_.emplace( id, std::move( reply ) );
			QTimer::singleShot( timeout_ms, this, [this, id, method = std::string( method ), timeout_ms] {
				if ( const auto it = pending_.find( id ); it != pending_.end() )
				{
					const Reply late = std::move( it->second );
					pending_.erase( it );
					log::warn( "the daemon did not answer {} within {} ms", method, timeout_ms );
					late( nullptr, QStringLiteral( "The daemon did not answer in time" ) );
				}
			} );
		}
		const auto message = ipc::request( id, method, params );
		socket_->write( message.data(), static_cast<qint64>( message.size() ) );
		bytes_sent_ += message.size();
	}

	void DaemonClient::read()
	{
		const QByteArray data = socket_->readAll();
		bytes_received_ += static_cast<std::uint64_t>( data.size() );
		buffer_.append( std::string_view( data.constData(), static_cast<std::size_t>( data.size() ) ) );
		if ( buffer_.overflowed() )
		{
			socket_->abort();
			return;
		}

		while ( auto line = buffer_.next() )
		{
			auto message = json::Document::parse( std::move( *line ) );
			if ( !message )
			{
				continue;
			}
			const json::Value& root = message->root();
			if ( const auto* name = root.find( "event" ); name != nullptr )
			{
				for ( const auto& handler : event_handlers_ )
				{
					handler( name->asString(), root["params"] );
				}
				continue;
			}

			const auto it = pending_.find( root["id"].asInt( -1 ) );
			if ( it == pending_.end() )
			{
				continue;
			}
			const Reply reply = std::move( it->second );
			pending_.erase( it );
			if ( const auto* error = root.find( "error" ); error != nullptr )
			{
				reply( nullptr, qs( ( *error )["message"].asString( "request failed" ) ) );
			}
			else
			{
				reply( &root["result"], {} );
			}
		}
	}

	void DaemonClient::failPending( const QString& reason )
	{
		auto pending = std::move( pending_ );
		pending_.clear();
		for ( auto& [id, reply] : pending )
		{
			reply( nullptr, reason );
		}
	}

	void DaemonClient::notifyConnection( bool connected )
	{
		log::info( "{} the daemon ({})", connected ? "connected to" : "disconnected from", paths::ipcEndpoint() );
		for ( const auto& handler : connection_handlers_ )
		{
			handler( connected );
		}
	}

} // namespace lexiglance::gui
