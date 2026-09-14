#include "Test.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

namespace lexiglance::test
{

	namespace
	{

		struct Case
		{
			std::string_view name;
			TestFunction     function = nullptr;
		};

		std::vector<Case>& registry()
		{
			static std::vector<Case> cases;
			return cases;
		}

		int& failures()
		{
			static int count = 0;
			return count;
		}

	} // namespace

	Registrar::Registrar( std::string_view name, TestFunction function )
	{
		registry().push_back( { .name = name, .function = function } );
	}

	void report( std::string_view message, std::source_location where )
	{
		++failures();
		std::println( stderr, "    {}:{}: {}", where.file_name(), where.line(), message );
	}

	std::filesystem::path fixtureDirectory()
	{
		return LEXIGLANCE_TEST_DATA;
	}

	std::filesystem::path fixtureZip()
	{
		return LEXIGLANCE_TEST_ZIP;
	}

	std::filesystem::path scratch( std::string_view name )
	{
		const auto path = std::filesystem::temp_directory_path() / std::format( "lexiglance-tests-{}", ::getpid() ) / name;
		std::filesystem::remove_all( path );
		std::filesystem::create_directories( path );
		return path;
	}

} // namespace lexiglance::test

namespace
{

	int run( std::span<char*> argv )
	{
		using namespace lexiglance::test;
		const std::string_view filter = argv.size() > 1 ? std::string_view( argv[1] ) : std::string_view();

		int failed_cases = 0;
		int ran          = 0;
		for ( const Case& test : registry() )
		{
			if ( !filter.empty() && !test.name.contains( filter ) )
			{
				continue;
			}
			const int  before  = failures();
			const auto started = std::chrono::steady_clock::now();
			try
			{
				test.function();
			}
			catch ( const std::exception& e )
			{
				report( std::format( "exception: {}", e.what() ), std::source_location::current() );
			}
			const auto ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
			++ran;
			if ( failures() != before )
			{
				++failed_cases;
				std::println( "FAIL  {} ({:.1f} ms)", test.name, ms );
			}
			else
			{
				std::println( "ok    {} ({:.1f} ms)", test.name, ms );
			}
		}

		std::error_code ec;
		std::filesystem::remove_all( std::filesystem::temp_directory_path() / std::format( "lexiglance-tests-{}", ::getpid() ), ec );
		std::println( "{} of {} tests passed", ran - failed_cases, ran );
		return failed_cases == 0 ? 0 : 1;
	}

} // namespace

int main( int argc, char** argv )
{
	try
	{
		return run( std::span<char*>( argv, static_cast<std::size_t>( argc ) ) );
	}
	catch ( ... )
	{
		return 2;
	}
}
