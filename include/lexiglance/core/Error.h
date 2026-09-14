#ifndef LEXIGLANCE_CORE_ERROR_H
#define LEXIGLANCE_CORE_ERROR_H

#include <expected>
#include <format>
#include <string>
#include <utility>

namespace lexiglance
{

	struct Error
	{
		std::string message;
	};

	template <typename T = void>
	using Result = std::expected<T, Error>;

	template <typename... Args>
	[[nodiscard]] std::unexpected<Error> fail( std::format_string<Args...> format, Args&&... args )
	{
		return std::unexpected( Error{ std::format( format, std::forward<Args>( args )... ) } );
	}

	[[nodiscard]] inline std::unexpected<Error> failWith( std::string_view context, const Error& error )
	{
		return std::unexpected( Error{ std::format( "{}: {}", context, error.message ) } );
	}

} // namespace lexiglance

#endif // LEXIGLANCE_CORE_ERROR_H
