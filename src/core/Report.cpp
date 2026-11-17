#include <lexiglance/core/Report.h>

#include <lexiglance/core/Health.h>
#include <lexiglance/core/Log.h>
#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Utf8.h>
#include <lexiglance/core/Version.h>
#include <lexiglance/core/Zip.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <utility>

#ifdef _WIN32
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <sys/utsname.h>
	#ifdef __APPLE__
		#include <sys/sysctl.h>
	#endif
#endif

namespace lexiglance::report
{

	namespace
	{

		std::string environment( const char* name )
		{
			const char* value = std::getenv( name );
			return value != nullptr ? std::string( value ) : std::string();
		}

		std::string join( const std::vector<std::string>& items, std::string_view between )
		{
			std::string out;
			for ( const auto& item : items )
			{
				out.append( out.empty() ? "" : between ).append( item );
			}
			return out;
		}

		// One line of at most `characters`, for the summary.
		std::string shortLine( std::string_view text, std::size_t characters )
		{
			std::string line( utf8::prefix( text, characters ) );
			std::ranges::replace( line, '\n', ' ' );
			return line.size() < text.size() ? line + "…" : line;
		}

		std::string percentEncoded( std::string_view text )
		{
			std::string out;
			for ( const char c : text )
			{
				const auto byte = static_cast<unsigned char>( c );
				if ( ( byte >= 'A' && byte <= 'Z' ) || ( byte >= 'a' && byte <= 'z' ) || ( byte >= '0' && byte <= '9' ) || c == '-' || c == '.' || c == '_' || c == '~' )
				{
					out.push_back( c );
				}
				else
				{
					out.append( std::format( "%{:02X}", byte ) );
				}
			}
			return out;
		}

#ifdef _WIN32
		std::string windowsVersion()
		{
			// RtlGetVersion tells the truth; GetVersionEx says 6.2 to programs without a manifest naming newer Windows.
			using Get = LONG( WINAPI* )( OSVERSIONINFOW* );
			OSVERSIONINFOW info{};
			info.dwOSVersionInfoSize = sizeof( info );
			HMODULE    ntdll         = GetModuleHandleW( L"ntdll.dll" );
			const auto get           = ntdll != nullptr ? reinterpret_cast<Get>( reinterpret_cast<void*>( GetProcAddress( ntdll, "RtlGetVersion" ) ) ) : nullptr;
			if ( get == nullptr || get( &info ) != 0 )
			{
				return "Windows";
			}
			SYSTEM_INFO system{};
			GetNativeSystemInfo( &system );
			const char* processor = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x64";
			return std::format( "Windows {} (build {}, {})", info.dwBuildNumber >= 22000 ? 11 : static_cast<int>( info.dwMajorVersion ), info.dwBuildNumber, processor );
		}
#endif

	} // namespace

	std::string system()
	{
#ifdef _WIN32
		return windowsVersion();
#else
		utsname    machine{};
		const bool known = ::uname( &machine ) == 0;
	#ifdef __APPLE__
		std::array<char, 64> release{};
		std::size_t          size    = release.size();
		const std::string    version = ::sysctlbyname( "kern.osproductversion", release.data(), &size, nullptr, 0 ) == 0 ? std::string( release.data() ) : std::string( "?" );
		return std::format( "macOS {} ({})", version, known ? std::string_view( machine.machine ) : std::string_view( "?" ) );
	#else
		std::string name = "Linux";
		for ( const char* file : { "/etc/os-release", "/usr/lib/os-release" } )
		{
			std::ifstream in( file );
			std::string   line;
			while ( std::getline( in, line ) )
			{
				if ( line.starts_with( "PRETTY_NAME=" ) )
				{
					name = line.substr( 12 );
					std::erase( name, '"' );
					break;
				}
			}
			if ( name != "Linux" )
			{
				break;
			}
		}
		std::vector<std::string> parts{ known ? std::format( "{} ({}, kernel {})", name, std::string_view( machine.machine ), std::string_view( machine.release ) ) : name };
		if ( const auto desktop = environment( "XDG_CURRENT_DESKTOP" ); !desktop.empty() )
		{
			parts.push_back( desktop );
		}
		std::string session = environment( "XDG_SESSION_TYPE" );
		if ( session.empty() || session == "tty" )
		{
			session = !environment( "WAYLAND_DISPLAY" ).empty() ? "wayland" : !environment( "DISPLAY" ).empty() ? "x11"
			                                                                                                    : session;
		}
		if ( !session.empty() )
		{
			parts.push_back( session + ( session == "wayland" && !environment( "DISPLAY" ).empty() ? " (with XWayland)" : "" ) );
		}
		return join( parts, ", " );
	#endif
#endif
	}

	std::string withoutSecrets( std::string_view url )
	{
		std::string out( url );
		// user:password@ in front of the host, up to the last @: a password may hold /, ? or # as typed. Text before an @
		// in the path or the query goes too when it has a colon (a port); a report loses nothing that matters by it.
		if ( const auto scheme = out.find( "://" ); scheme != std::string::npos )
		{
			const auto start = scheme + 3;
			const auto at    = out.rfind( '@' );
			if ( at != std::string::npos && at > start )
			{
				const std::string_view before = std::string_view( out ).substr( start, at - start );
				if ( before.contains( ':' ) || before.find_first_of( "/?#" ) == std::string_view::npos )
				{
					out.replace( start, at - start, "<redacted>" );
				}
			}
		}
		// The values of parameters whose name says they are secret.
		static constexpr std::array<std::string_view, 6> secret{ "key", "token", "secret", "pass", "auth", "sig" };
		for ( std::size_t at = out.find_first_of( "?&" ); at != std::string::npos; at = out.find( '&', at + 1 ) )
		{
			const auto equals = out.find( '=', at );
			const auto end    = out.find_first_of( "&#", at + 1 );
			if ( equals == std::string::npos || ( end != std::string::npos && equals > end ) )
			{
				continue;
			}
			std::string name = out.substr( at + 1, equals - at - 1 );
			std::ranges::transform( name, name.begin(), []( char c ) { return static_cast<char>( c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c ); } );
			if ( std::ranges::any_of( secret, [&]( std::string_view word ) { return name.contains( word ); } ) )
			{
				out.replace( equals + 1, ( end == std::string::npos ? out.size() : end ) - equals - 1, "<redacted>" );
			}
		}
		return out;
	}

	std::string redactedConfig( config::Config config )
	{
		if ( !config.anki.key.empty() )
		{
			config.anki.key = "<redacted>";
		}
		config.anki.url = withoutSecrets( config.anki.url );
		for ( auto& source : config.audio.sources )
		{
			source = withoutSecrets( source );
		}
		return config.toJson( true );
	}

	std::string withoutHome( std::string text )
	{
#ifdef _WIN32
		const std::string home = environment( "USERPROFILE" );
#else
		const std::string home = environment( "HOME" );
#endif
		if ( home.size() < 2 )
		{
			return text;
		}
#ifdef _WIN32
		// JSON writes its backslashes twice and Qt writes paths with forward slashes; the daemon's pipe is named after
		// the account, as the home folder is.
		std::string escaped;
		for ( const char c : home )
		{
			escaped.append( c == '\\' ? "\\\\" : std::string( 1, c ) );
		}
		std::string forward = home;
		std::ranges::replace( forward, '\\', '/' );
		std::vector<std::pair<std::string, std::string>> spellings{ { home, "~" }, { escaped, "~" }, { forward, "~" } };
		if ( const std::string user = environment( "USERNAME" ); !user.empty() )
		{
			spellings.emplace_back( "lexiglance-" + user, "lexiglance-~" );
			// The settings window's own name (lexiglance-gui-<account>), logged when it cannot take it.
			spellings.emplace_back( "lexiglance-gui-" + user, "lexiglance-gui-~" );
		}
#else
		const std::vector<std::pair<std::string, std::string>> spellings{ { home, "~" } };
#endif
		for ( const auto& [spelling, replacement] : spellings )
		{
			for ( std::size_t at = text.find( spelling ); at != std::string::npos; at = text.find( spelling, at + replacement.size() ) )
			{
				text.replace( at, spelling.size(), replacement );
			}
		}
		return text;
	}

	std::vector<std::string> tail( const std::filesystem::path& file, std::size_t count )
	{
		std::ifstream           in( file, std::ios::binary );
		std::deque<std::string> lines;
		std::string             line;
		while ( count > 0 && std::getline( in, line ) )
		{
			if ( !line.empty() && line.back() == '\r' )
			{
				line.pop_back();
			}
			// A log cut off by a crash ends in NUL bytes.
			std::erase( line, '\0' );
			if ( lines.size() == count )
			{
				lines.pop_front();
			}
			lines.push_back( std::move( line ) );
		}
		return { std::make_move_iterator( lines.begin() ), std::make_move_iterator( lines.end() ) };
	}

	std::string summary( const Input& input )
	{
		std::vector<std::string> lines{ input.program };
		lines.push_back( "System: " + system() );
		if ( input.status == nullptr )
		{
			lines.emplace_back( "Daemon: not running" );
		}
		else
		{
			const json::Value& status = *input.status;
			lines.push_back( std::format( "Daemon: {} ({} backend), scale {:.3g}x, {} theme, {}", status["version"].asString( "?" ), status["backend"].asString( "?" ), status["scale"].asDouble( 1.0 ), status["dark_theme"].asBool() ? "dark" : "light", status["transparency"].asBool() ? "compositing" : "no compositor" ) );
			lines.push_back( std::format( "Text capture: {}", status["capture"].asString( "?" ) ) );
			if ( const auto problem = status["capture_problem"].asString(); !problem.empty() )
			{
				lines.push_back( "Capture problem: " + shortLine( problem, 300 ) );
			}
			std::vector<std::string> languages;
			for ( const json::Value& code : status["languages"].items() )
			{
				languages.emplace_back( code.asString() );
			}
			lines.push_back( std::format( "Languages: {} · {} dictionaries{}", languages.empty() ? std::string( "none" ) : join( languages, ", " ), status["dictionaries"].asInt(), status["paused"].asBool() ? " · scanning paused" : "" ) );
		}
		if ( input.health != nullptr )
		{
			const auto checks   = health::read( *input.health );
			const auto errors   = std::ranges::count( checks, health::Severity::Error, &health::Check::status );
			const auto warnings = std::ranges::count( checks, health::Severity::Warning, &health::Check::status );
			lines.push_back( std::format( "Health: {} errors, {} warnings", errors, warnings ) );
			for ( const auto& check : checks )
			{
				if ( check.status == health::Severity::Error || check.status == health::Severity::Warning )
				{
					lines.push_back( std::format( "  {}: {}: {}", health::statusName( check.status ), check.title, shortLine( check.detail, 240 ) ) );
				}
			}
		}
		return withoutHome( join( lines, "\n" ) );
	}

	std::vector<Part> parts( const Input& input )
	{
		std::vector<Part> out;
		out.push_back( { .name = "report.txt", .title = "What happened, and the key facts", .contents = std::format( "What happened:\n{}\n\n{}\n\nMade {} by {}.\n", input.what.empty() ? std::string( "(not said)" ) : input.what, summary( input ), log::timestamp( std::chrono::system_clock::now() ), input.program ) } );
		const auto add_json = [&]( const json::Value* value, std::string name, std::string title ) {
			if ( value != nullptr )
			{
				out.push_back( { .name = std::move( name ), .title = std::move( title ), .contents = withoutHome( json::serialize( *value ) ) + "\n" } );
			}
		};
		add_json( input.status, "status.json", "The daemon's status: versions, backend, text capture, scale" );
		add_json( input.health, "health.json", "The health report" );

		if ( std::filesystem::is_regular_file( paths::configFile() ) )
		{
			auto config = config::Config::load( paths::configFile() );
			out.push_back( { .name = "config.json", .title = "Settings, with the AnkiConnect key and other secrets removed", .contents = config ? withoutHome( redactedConfig( std::move( *config ) ) ) + "\n" : "unreadable: " + config.error().message + "\n" } );
		}

		if ( input.dictionaries != nullptr )
		{
			std::string list;
			for ( const json::Value& d : ( *input.dictionaries )["dictionaries"].items() )
			{
				list.append( std::format( "{} [{}] {} {}->{}, {} terms, {} kanji, {} frequencies/pitch{}\n", d["title"].asString(), d["revision"].asString(), d["kind"].asString(), d["source_language"].asString( "?" ), d["target_language"].asString( "?" ), d["terms"].asInt(), d["kanji"].asInt(), d["meta"].asInt(), d["enabled"].asBool() ? "" : " (turned off)" ) );
			}
			out.push_back( { .name = "dictionaries.txt", .title = "Installed dictionaries: names and languages, not their contents", .contents = list.empty() ? "none\n" : list } );
		}

		const auto add_log = [&]( const char* file, std::string title ) {
			const auto lines = tail( paths::stateDir() / file, input.log_lines );
			if ( !lines.empty() )
			{
				out.push_back( { .name = file, .title = std::format( "{} (the last {} lines)", title, lines.size() ), .contents = withoutHome( join( lines, "\n" ) ) + "\n" } );
			}
		};
		add_log( "daemon.log", "The daemon's log" );
		add_log( "application.log", "The settings application's log" );
		return out;
	}

	std::string issueUrl( std::string_view what, std::string_view summary )
	{
		constexpr std::size_t most      = 7000;
		const std::string     version_s = commit.empty() ? std::format( "{} ({})", version, channel ) : std::format( "{} ({} {})", version, channel, commit );
		const std::string     start     = std::format(
				"{}/issues/new?template=bug_report.yml&labels=bug&title={}&version={}&system={}",
				project_url,
				percentEncoded( utf8::prefix( what.substr( 0, what.find( '\n' ) ), 70 ) ),
				percentEncoded( version_s ),
				percentEncoded( system() )
		);
		// The whole text is in the zip; the form gets what fits.
		std::string said( utf8::prefix( what, 2000 ) );
		std::string facts( summary );
		while ( true )
		{
			const std::string url = std::format( "{}&what={}&summary={}", start, percentEncoded( said ), percentEncoded( facts ) );
			if ( url.size() <= most || ( facts.empty() && said.empty() ) )
			{
				return url;
			}
			// The summary's last lines go first (it is in the zip whole), then the end of what was said.
			if ( !facts.empty() )
			{
				const auto cut = facts.rfind( '\n' );
				facts.resize( cut == std::string::npos ? 0 : cut );
			}
			else
			{
				said = std::string( utf8::prefix( said, utf8::length( said ) * 3 / 4 ) );
			}
		}
	}

	Result<> save( const std::filesystem::path& zip, std::span<const Part> parts )
	{
		std::error_code ec;
		std::filesystem::create_directories( zip.parent_path(), ec );
		ZipWriter writer( zip );
		for ( const Part& part : parts )
		{
			if ( auto added = writer.add( part.name, std::as_bytes( std::span( part.contents.data(), part.contents.size() ) ) ); !added )
			{
				return added;
			}
		}
		return writer.close();
	}

	std::string fileName( std::chrono::system_clock::time_point when )
	{
		// "2026-11-17 20:14:30.123"
		std::string stamp = log::timestamp( when ).substr( 0, 19 );
		std::erase( stamp, '-' );
		std::erase( stamp, ':' );
		std::ranges::replace( stamp, ' ', '-' );
		return std::format( "lexiglance-report-{}.zip", stamp );
	}

} // namespace lexiglance::report
