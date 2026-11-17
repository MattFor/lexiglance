#ifndef LEXIGLANCE_CORE_COMPAT_H
#define LEXIGLANCE_CORE_COMPAT_H

// Standard library pieces Apple's libc++ (LLVM 19, Xcode 16) does not have yet. Elsewhere these are the standard ones.

#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace lexiglance
{

#ifdef __cpp_lib_move_only_function
	using Task = std::move_only_function<void()>;
#else
	// ponytail: a void() stand-in for std::move_only_function; drop it once Xcode's libc++ has the real one.
	class Task
	{
	public:
		Task() noexcept = default;

		template <typename F>
			requires( !std::same_as<std::remove_cvref_t<F>, Task> && std::invocable<std::decay_t<F>&> )
		Task( F&& function ) : // NOLINT(google-explicit-constructor): converts like std::move_only_function
			callable_( std::make_unique<Model<std::decay_t<F>>>( std::forward<F>( function ) ) )
		{
		}

		void operator()()
		{
			callable_->call();
		}

		explicit operator bool() const noexcept
		{
			return callable_ != nullptr;
		}

	private:
		struct Callable
		{
			virtual ~Callable() = default;
			virtual void call() = 0;
		};

		template <typename F>
		struct Model final : Callable
		{
			template <typename G>
			explicit Model( G&& function ) :
				function( std::forward<G>( function ) )
			{
			}

			void call() override
			{
				std::invoke( function );
			}

			F function;
		};

		std::unique_ptr<Callable> callable_;
	};
#endif

#ifdef __cpp_lib_atomic_shared_ptr
	template <typename T>
	using AtomicShared = std::atomic<std::shared_ptr<T>>;
#else
	// ponytail: std::atomic<std::shared_ptr> behind a mutex; drop it once Xcode's libc++ has the real one.
	template <typename T>
	class AtomicShared
	{
	public:
		AtomicShared() = default;
		explicit AtomicShared( std::shared_ptr<T> value ) :
			value_( std::move( value ) )
		{
		}

		[[nodiscard]] std::shared_ptr<T> load() const
		{
			const std::scoped_lock lock( mutex_ );
			return value_;
		}

		void store( std::shared_ptr<T> value )
		{
			const std::scoped_lock lock( mutex_ );
			value_.swap( value );
		}

	private:
		mutable std::mutex mutex_;
		std::shared_ptr<T> value_;
	};
#endif

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_COMPAT_H
