#include <lexiglance/dictionary/WordRules.h>

namespace lexiglance::dict::rule
{

	namespace
	{

		std::uint32_t parseOne( std::string_view name ) noexcept
		{
			if ( name.starts_with( "v1" ) )
			{
				return v1;
			}
			if ( name.starts_with( "v5" ) )
			{
				return v5;
			}
			if ( name == "vs" || name.starts_with( "vs-" ) )
			{
				return vs;
			}
			if ( name == "vk" )
			{
				return vk;
			}
			if ( name == "vz" )
			{
				return vz;
			}
			if ( name == "adj-i" || name == "adj-ix" )
			{
				return adj_i;
			}
			if ( name == "n" || name == "noun" )
			{
				return noun;
			}
			if ( name == "v" || name == "verb" )
			{
				return verb;
			}
			if ( name == "adj" || name == "adjective" )
			{
				return adjective;
			}
			return 0;
		}

	} // namespace

	std::uint32_t parse( std::string_view rules ) noexcept
	{
		std::uint32_t bits = 0;
		while ( !rules.empty() )
		{
			const auto space = rules.find( ' ' );
			bits |= parseOne( rules.substr( 0, space ) );
			if ( space == std::string_view::npos )
			{
				break;
			}
			rules.remove_prefix( space + 1 );
		}
		return bits;
	}

} // namespace lexiglance::dict::rule
