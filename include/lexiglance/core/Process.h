#ifndef LEXIGLANCE_CORE_PROCESS_H
#define LEXIGLANCE_CORE_PROCESS_H

#include <chrono>
#include <filesystem>
#include <string_view>
#include <vector>

// Processes of the current user: keeping the daemon to a single instance, and finding or ending stray ones.
namespace lexiglance::process
{

	// An exclusive per-user lock held for the life of a process: flock() on a file that records the holder's pid. Two
	// daemons starting at the same moment can never both run, and the kernel releases the lock however the holder ends.
	class InstanceLock
	{
	public:
		InstanceLock() = default;
		// Tries to take the lock at once.
		explicit InstanceLock( std::filesystem::path file );
		~InstanceLock();

		InstanceLock( const InstanceLock& )            = delete;
		InstanceLock& operator=( const InstanceLock& ) = delete;
		InstanceLock( InstanceLock&& other ) noexcept;
		InstanceLock& operator=( InstanceLock&& other ) noexcept;

		[[nodiscard]] bool held() const noexcept
		{
			return held_;
		}

		// The pid the holding process recorded (0 when unknown); meaningful while the lock is not held.
		[[nodiscard]] int holder() const;

		// Tries again, e.g. after the holder was asked to exit.
		bool retry();

	private:
		void release() noexcept;

		std::filesystem::path file_;
		int                   fd_   = -1;
		bool                  held_ = false;
	};

	// Whether `pid` is a live (not zombie) process of this user running the executable `name` (on Windows without
	// ".exe").
	[[nodiscard]] bool isRunning( int pid, std::string_view name );

	// Whether a process runs as the current user (a daemon to end, the other end of a pipe).
	[[nodiscard]] bool sameUser( int pid );

	// The other live processes of this user running `name`; empty where that cannot be told (no /proc).
	[[nodiscard]] std::vector<int> othersNamed( std::string_view name );

	// Asks a process running `name` to end (SIGTERM) and kills it (SIGKILL) if it is still there after `grace`.
	// True once it is gone.
	bool terminate( int pid, std::string_view name, std::chrono::milliseconds grace );

	// The executable of this process.
	[[nodiscard]] std::filesystem::path executable();

	// Whether the executable file was replaced (rebuilt or updated) after this process started.
	[[nodiscard]] bool executableReplaced();

} // namespace lexiglance::process

#endif // LEXIGLANCE_CORE_PROCESS_H
