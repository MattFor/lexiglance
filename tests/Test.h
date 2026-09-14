#ifndef LEXIGLANCE_TESTS_TEST_H
#define LEXIGLANCE_TESTS_TEST_H

#include <filesystem>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

// Minimal self-registering test harness (no macros, no dependencies).
namespace lexiglance::test
{

	using TestFunction = void ( * )();

	struct Registrar
	{
		Registrar( std::string_view name, TestFunction function );
	};

	void report( std::string_view message, std::source_location where );

	inline bool expect( bool condition, std::source_location where = std::source_location::current() )
	{
		if ( !condition )
		{
			report( "expectation failed", where );
		}
		return condition;
	}

	template <typename A, typename B>
	bool expectEqual( const A& actual, const B& expected, std::source_location where = std::source_location::current() )
	{
		if ( actual == expected )
		{
			return true;
		}
		if constexpr ( std::formattable<A, char> && std::formattable<B, char> )
		{
			report( std::format( R"(expected "{}", got "{}")", expected, actual ), where );
		}
		else
		{
			report( "values differ", where );
		}
		return false;
	}

	[[nodiscard]] std::filesystem::path fixtureDirectory();
	[[nodiscard]] std::filesystem::path fixtureZip();

	// Fresh directory for the files a test writes.
	[[nodiscard]] std::filesystem::path scratch( std::string_view name );

} // namespace lexiglance::test

#endif // LEXIGLANCE_TESTS_TEST_H
