#ifndef LEXIGLANCE_DAEMON_CONFIGWATCH_H
#define LEXIGLANCE_DAEMON_CONFIGWATCH_H

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>

namespace lexiglance::daemon
{

	// Tells a settings file changed by someone else (edited by hand, synced from another computer) from the daemon's own
	// saves, by its time and size.
	class ConfigWatch
	{
	public:
		explicit ConfigWatch( std::filesystem::path file ) :
			file_( std::move( file ) ),
			known_( stamp() )
		{
		}

		// The file as it is now is the daemon's own: just saved, or read.
		void own()
		{
			known_ = stamp();
		}

		// Whether someone else changed the file since it was last owned or reported. A file gone missing is no change: some
		// editors remove it for a moment while saving, and the settings in use stay meanwhile.
		[[nodiscard]] bool pending() const
		{
			const Stamp now = stamp();
			return now.exists && now != known_;
		}

		// pending(), and the change is known from then on.
		[[nodiscard]] bool changed()
		{
			if ( !pending() )
			{
				return false;
			}
			known_ = stamp();
			return true;
		}

	private:
		struct Stamp
		{
			std::filesystem::file_time_type time;
			std::uintmax_t                  size   = 0;
			bool                            exists = false;

			[[nodiscard]] bool operator==( const Stamp& ) const = default;
		};

		[[nodiscard]] Stamp stamp() const
		{
			std::error_code ec;
			Stamp           stamp{ .time = std::filesystem::last_write_time( file_, ec ) };
			if ( ec )
			{
				return {};
			}
			stamp.size   = std::filesystem::file_size( file_, ec );
			stamp.exists = !ec;
			return stamp;
		}

		std::filesystem::path file_;
		Stamp                 known_;
	};

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_CONFIGWATCH_H
