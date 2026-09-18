#ifndef LEXIGLANCE_GUI_SETTINGS_H
#define LEXIGLANCE_GUI_SETTINGS_H

#include <lexiglance/config/Config.h>

#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace lexiglance::gui
{

	class DaemonClient;

	// Working copy of the daemon's configuration. Edits are applied instantly: commit() pushes them (debounced) and the
	// daemon persists and applies them live.
	class Settings
	{
	public:
		explicit Settings( DaemonClient& client );

		[[nodiscard]] config::Config& config() noexcept
		{
			return config_;
		}

		[[nodiscard]] bool loaded() const noexcept
		{
			return loaded_;
		}

		void load();
		void commit();

		// Replaces the whole working copy (with defaults, say): every page shows it and the daemon gets it.
		void replace( config::Config config );

		void onChanged( std::function<void()> handler )
		{
			handlers_.push_back( std::move( handler ) );
		}

	private:
		void push();

		DaemonClient*                         client_;
		config::Config                        config_;
		std::unique_ptr<QTimer>               timer_;
		bool                                  loaded_  = false;
		bool                                  pending_ = false;
		std::chrono::steady_clock::time_point pushed_at_;
		std::vector<std::function<void()>>    handlers_;
	};

} // namespace lexiglance::gui

#endif // LEXIGLANCE_GUI_SETTINGS_H
