#ifndef LEXIGLANCE_CORE_THREAD_H
#define LEXIGLANCE_CORE_THREAD_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lexiglance
{

	namespace thread
	{

		void setName( std::string_view name ) noexcept;

		// Lowers the calling thread's scheduling priority so bulk work never competes with games or the desktop.
		void setBackgroundPriority() noexcept;

	} // namespace thread

	class ThreadPool
	{
	public:
		explicit ThreadPool( unsigned threads = 0, bool background = false, std::string_view name = "worker" );
		~ThreadPool();

		ThreadPool( const ThreadPool& )            = delete;
		ThreadPool& operator=( const ThreadPool& ) = delete;
		ThreadPool( ThreadPool&& )                 = delete;
		ThreadPool& operator=( ThreadPool&& )      = delete;

		template <typename F>
		[[nodiscard]] auto submit( F&& task ) -> std::future<std::invoke_result_t<std::decay_t<F>>>
		{
			using R = std::invoke_result_t<std::decay_t<F>>;
			std::packaged_task<R()> packaged( std::forward<F>( task ) );
			auto                    future = packaged.get_future();
			enqueue( [job = std::move( packaged )]() mutable { job(); } );
			return future;
		}

		[[nodiscard]] unsigned size() const noexcept
		{
			return static_cast<unsigned>( workers_.size() );
		}

	private:
		void enqueue( std::move_only_function<void()> job );
		void work( const std::stop_token& stop );

		std::mutex                                  mutex_;
		std::condition_variable_any                 ready_;
		std::deque<std::move_only_function<void()>> queue_;
		std::vector<std::jthread>                   workers_;
	};

	// Single slot channel where a newer value replaces an unconsumed one. Used between pipeline stages so that fast
	// pointer movement never builds a backlog: only the latest request is ever processed.
	template <typename T>
	class Mailbox
	{
	public:
		void post( T value )
		{
			{
				const std::scoped_lock lock( mutex_ );
				slot_ = std::move( value );
			}
			ready_.notify_one();
		}

		[[nodiscard]] std::optional<T> wait( const std::stop_token& stop )
		{
			std::unique_lock lock( mutex_ );
			if ( !ready_.wait( lock, stop, [this] { return slot_.has_value(); } ) )
			{
				return std::nullopt;
			}
			return std::exchange( slot_, std::nullopt );
		}

		// As wait(), but gives up after `timeout`.
		[[nodiscard]] std::optional<T> wait( const std::stop_token& stop, std::chrono::milliseconds timeout )
		{
			std::unique_lock lock( mutex_ );
			if ( !ready_.wait_for( lock, stop, timeout, [this] { return slot_.has_value(); } ) )
			{
				return std::nullopt;
			}
			return std::exchange( slot_, std::nullopt );
		}

	private:
		std::mutex                  mutex_;
		std::condition_variable_any ready_;
		std::optional<T>            slot_;
	};

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_THREAD_H
