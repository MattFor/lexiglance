#include <lexiglance/core/Thread.h>

#include <algorithm>
#include <array>
#include <string>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <pthread.h>
	#include <sys/resource.h>
	#include <unistd.h>
#endif
#ifdef __APPLE__
	#include <pthread/qos.h>
#endif
#ifdef __FreeBSD__
	#include <pthread_np.h>
#endif

namespace lexiglance
{

	namespace thread
	{

		namespace
		{

			// What this thread is called here, whatever the system made of it (Linux keeps 15 characters).
			std::string& nameStorage() noexcept
			{
				static thread_local std::string value;
				return value;
			}

		} // namespace

		std::string_view name() noexcept
		{
			return nameStorage();
		}

		void setName( std::string_view name ) noexcept
		{
			nameStorage().assign( name );
#ifdef _WIN32
			const std::wstring wide( name.begin(), name.end() );
			( void )SetThreadDescription( GetCurrentThread(), wide.c_str() );
#elifdef __APPLE__
			const std::string owned( name.substr( 0, 63 ) );
			( void )pthread_setname_np( owned.c_str() );
#elif defined( __linux__ ) || defined( __FreeBSD__ )
	#ifdef __linux__
			const bool main_thread = ::gettid() == ::getpid();
	#else
			const bool main_thread = pthread_main_np() != 0;
	#endif
			if ( main_thread )
			{
				return;
			}
			// The kernel limits thread names to 15 characters.
			std::array<char, 16> buffer{};
			( void )name.copy( buffer.data(), buffer.size() - 1 );
			( void )pthread_setname_np( pthread_self(), buffer.data() );
#else
			( void )name;
#endif
		}

		void setBackgroundPriority() noexcept
		{
#ifdef _WIN32
			SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
#elifdef __APPLE__
			( void )pthread_set_qos_class_self_np( QOS_CLASS_UTILITY, 0 );
#elifdef __linux__
			// On Linux nice values are per thread.
			( void )setpriority( PRIO_PROCESS, static_cast<id_t>( gettid() ), 10 );
#endif
		}

	} // namespace thread

	ThreadPool::ThreadPool( unsigned threads, bool background, std::string_view name )
	{
		if ( threads == 0 )
		{
			threads = std::max( 1U, std::thread::hardware_concurrency() );
		}

		workers_.reserve( threads );
		for ( unsigned i = 0; i < threads; ++i )
		{
			workers_.emplace_back( [this, background, thread_name = std::string( name )]( const std::stop_token& stop ) {
				thread::setName( thread_name );
				if ( background )
				{
					thread::setBackgroundPriority();
				}
				work( stop );
			} );
		}
	}

	ThreadPool::~ThreadPool()
	{
		for ( auto& worker : workers_ )
		{
			worker.request_stop();
		}
		workers_.clear();
	}

	void ThreadPool::enqueue( std::move_only_function<void()> job )
	{
		{
			const std::scoped_lock lock( mutex_ );
			queue_.push_back( std::move( job ) );
		}
		ready_.notify_one();
	}

	void ThreadPool::work( const std::stop_token& stop )
	{
		while ( true )
		{
			std::move_only_function<void()> job;
			{
				std::unique_lock lock( mutex_ );
				if ( !ready_.wait( lock, stop, [this] { return !queue_.empty(); } ) )
				{
					return;
				}
				job = std::move( queue_.front() );
				queue_.pop_front();
			}
			job();
		}
	}

} // namespace lexiglance
