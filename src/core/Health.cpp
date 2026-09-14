#include <lexiglance/core/Health.h>

#include <algorithm>

namespace lexiglance::health
{

	std::string_view statusName( Severity status ) noexcept
	{
		switch ( status )
		{
			case Severity::Info:
				return "info";
			case Severity::Warning:
				return "warning";
			case Severity::Error:
				return "error";
			case Severity::Ok:
				break;
		}
		return "ok";
	}

	Severity parseStatus( std::string_view name ) noexcept
	{
		if ( name == "info" )
		{
			return Severity::Info;
		}
		if ( name == "warning" )
		{
			return Severity::Warning;
		}
		if ( name == "error" )
		{
			return Severity::Error;
		}
		return Severity::Ok;
	}

	void write( json::Writer& out, const std::vector<Check>& checks )
	{
		out.beginObject().key( "checks" ).beginArray();
		for ( const Check& check : checks )
		{
			out.beginObject()
					.field( "id", check.id )
					.field( "title", check.title )
					.field( "status", statusName( check.status ) )
					.field( "detail", check.detail )
					.field( "fix", check.fix )
					.endObject();
		}
		out.endArray();
		out.field( "errors", std::ranges::count( checks, Severity::Error, &Check::status ) );
		out.field( "warnings", std::ranges::count( checks, Severity::Warning, &Check::status ) );
		out.endObject();
	}

	std::vector<Check> read( const json::Value& report )
	{
		std::vector<Check> checks;
		for ( const json::Value& item : report["checks"].items() )
		{
			checks.push_back(
					{
							.id     = std::string( item["id"].asString() ),
							.title  = std::string( item["title"].asString() ),
							.status = parseStatus( item["status"].asString() ),
							.detail = std::string( item["detail"].asString() ),
							.fix    = std::string( item["fix"].asString() ),
					}
			);
		}
		return checks;
	}

} // namespace lexiglance::health
