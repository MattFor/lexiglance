#include "Settings.h"

#include "DaemonClient.h"

namespace lexiglance::gui
{

	Settings::Settings( DaemonClient& client ) :
		client_( &client ),
		timer_( std::make_unique<QTimer>() )
	{
		timer_->setSingleShot( true );
		timer_->setInterval( 350 );
		QObject::connect( timer_.get(), &QTimer::timeout, [this] { push(); } );

		client.onConnection( [this]( bool connected ) {
			if ( connected )
			{
				load();
			}
		} );
		client.onEvent( [this]( std::string_view name, const json::Value& ) {
			// Our own pushes come back as config.changed; only reload for changes made elsewhere.
			if ( ( name == "config.changed" || name == "dictionaries.changed" ) && std::chrono::steady_clock::now() - pushed_at_ > std::chrono::milliseconds( 1500 ) )
			{
				load();
			}
		} );
	}

	void Settings::load()
	{
		client_->call( "config.get", "{}", [this]( const json::Value* result, const QString& ) {
			if ( result == nullptr )
			{
				return;
			}
			if ( auto parsed = config::Config::fromJson( ( *result )["config"] ) )
			{
				// Local edits from setup (or a page) that committed before the first load must not be wiped.
				if ( !pending_ )
				{
					config_ = std::move( *parsed );
				}
				loaded_ = true;
				for ( const auto& handler : handlers_ )
				{
					handler();
				}
				if ( pending_ )
				{
					timer_->start();
				}
			}
		} );
	}

	void Settings::commit()
	{
		pending_ = true;
		if ( loaded_ )
		{
			timer_->start();
		}
	}

	void Settings::replace( config::Config config )
	{
		config_ = std::move( config );
		for ( const auto& handler : handlers_ )
		{
			handler();
		}
		commit();
	}

	void Settings::push()
	{
		pushed_at_ = std::chrono::steady_clock::now();
		pending_   = false;
		client_->call( "config.set", "{\"config\":" + config_.toJson( false ) + "}" );
	}

} // namespace lexiglance::gui
