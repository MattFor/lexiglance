#ifndef LEXIGLANCE_GUI_DAEMONCLIENT_H
#define LEXIGLANCE_GUI_DAEMONCLIENT_H

#include <lexiglance/core/Json.h>
#include <lexiglance/ipc/Protocol.h>

#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lexiglance::gui
{

	// Asynchronous connection to lexiglanced; reconnects automatically.
	class DaemonClient : public QObject
	{
	public:
		using Reply        = std::function<void( const json::Value* result, const QString& error )>;
		using EventHandler = std::function<void( std::string_view name, const json::Value& params )>;

		explicit DaemonClient( QObject* parent = nullptr );

		// A reply that does not come within `timeout_ms` is given up (a daemon that hangs must not freeze the settings).
		void call( std::string_view method, const std::string& params = "{}", Reply reply = {}, int timeout_ms = 15000 );

		void onEvent( EventHandler handler )
		{
			event_handlers_.push_back( std::move( handler ) );
		}

		void onConnection( std::function<void( bool )> handler )
		{
			connection_handlers_.push_back( std::move( handler ) );
		}

		[[nodiscard]] bool connected() const
		{
			return socket_->state() == QLocalSocket::ConnectedState;
		}

		void connectNow();

		// Launches lexiglanced detached; the connection follows automatically. With `replace` it takes over from a
		// running daemon, asking it to exit and ending it if it hangs, so it also recovers from a stuck one.
		bool startDaemon( bool replace = false );

		// Reconnects often for a while: a daemon is on its way.
		void expectDaemon();

		// The pid of the daemon connected to, from its status (0 when unknown).
		[[nodiscard]] qint64 daemonPid() const noexcept
		{
			return daemon_pid_;
		}

		void setDaemonPid( qint64 pid ) noexcept
		{
			daemon_pid_ = pid;
		}

		// Bytes sent to and received from the daemon since the settings application started.
		[[nodiscard]] std::uint64_t bytesSent() const noexcept
		{
			return bytes_sent_;
		}

		[[nodiscard]] std::uint64_t bytesReceived() const noexcept
		{
			return bytes_received_;
		}

	private:
		void read();
		void failPending( const QString& reason );
		void notifyConnection( bool connected );

		QLocalSocket*                            socket_;
		QTimer*                                  retry_;
		qint64                                   daemon_pid_     = 0;
		std::uint64_t                            bytes_sent_     = 0;
		std::uint64_t                            bytes_received_ = 0;
		std::chrono::steady_clock::time_point    fast_until_;
		ipc::LineBuffer                          buffer_;
		std::unordered_map<std::int64_t, Reply>  pending_;
		std::int64_t                             next_id_ = 1;
		std::vector<EventHandler>                event_handlers_;
		std::vector<std::function<void( bool )>> connection_handlers_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_DAEMONCLIENT_H
