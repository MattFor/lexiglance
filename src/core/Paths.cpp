#include <lexiglance/core/Paths.h>

#include <lexiglance/core/Hash.h>

#include <cstdint>
#include <cstdlib>
#include <format>
#include <string_view>

#ifndef _WIN32
	#include <unistd.h>
#endif

namespace lexiglance::paths
{

	namespace
	{

		namespace fs = std::filesystem;

		fs::path env( const char* name )
		{
			const char* value = std::getenv( name );
			return value != nullptr && *value != '\0' ? fs::path( value ) : fs::path();
		}

#ifndef _WIN32
		fs::path home()
		{
			return env( "HOME" );
		}
#endif

		fs::path portableRoot()
		{
			return env( "LEXIGLANCE_HOME" );
		}

		// Resolves a base directory: the portable root wins, then the platform variable, then the fallback below $HOME.
		fs::path resolve( std::string_view portable_sub, [[maybe_unused]] const char* xdg_variable, [[maybe_unused]] std::string_view xdg_fallback )
		{
			if ( const auto root = portableRoot(); !root.empty() )
			{
				return root / portable_sub;
			}

#ifdef _WIN32
			// Settings roam with the profile; dictionaries and OCR models are large and stay on this machine.
			const bool roaming = portable_sub == "config";
			return env( roaming ? "APPDATA" : "LOCALAPPDATA" ) / "Lexiglance" / portable_sub;
#elifdef __APPLE__
			if ( portable_sub == "cache" )
			{
				return home() / "Library" / "Caches" / "Lexiglance";
			}
			return home() / "Library" / "Application Support" / "Lexiglance" / portable_sub;
#else
			auto base = env( xdg_variable );
			if ( base.empty() || base.is_relative() )
			{
				base = home() / xdg_fallback;
			}
			return base / "lexiglance";
#endif
		}

	} // namespace

	fs::path configDir()
	{
		return resolve( "config", "XDG_CONFIG_HOME", ".config" );
	}

	fs::path dataDir()
	{
		return resolve( "data", "XDG_DATA_HOME", ".local/share" );
	}

	fs::path cacheDir()
	{
		return resolve( "cache", "XDG_CACHE_HOME", ".cache" );
	}

	fs::path stateDir()
	{
		return resolve( "state", "XDG_STATE_HOME", ".local/state" );
	}

	fs::path runtimeDir()
	{
		if ( const auto root = portableRoot(); !root.empty() )
		{
			return root / "run";
		}
#ifdef _WIN32
		return cacheDir() / "run";
#else
		if ( auto runtime = env( "XDG_RUNTIME_DIR" ); !runtime.empty() )
		{
			return runtime / "lexiglance";
		}
		return fs::temp_directory_path() / ( "lexiglance-" + std::to_string( ::getuid() ) );
#endif
	}

	fs::path configFile()
	{
		return configDir() / "config.json";
	}

	fs::path dictionariesDir()
	{
		return dataDir() / "dictionaries";
	}

	fs::path ocrDir()
	{
		return dataDir() / "ocr";
	}

	std::string ipcEndpoint()
	{
#ifdef _WIN32
		const auto user = env( "USERNAME" );
		return "\\\\.\\pipe\\lexiglance-" + user.string();
#else
		// sockaddr_un holds at most 107 bytes; deep runtime directories fall back to a short, stable temp path.
		auto socket = runtimeDir() / "daemon.sock";
		if ( socket.native().size() >= 100 )
		{
			const auto id = static_cast<std::uint32_t>( hash64( runtimeDir().native() ) );
			socket        = fs::temp_directory_path() / std::format( "lexiglance-{}-{:08x}", ::getuid(), id ) / "daemon.sock";
		}
		return socket.string();
#endif
	}

	Result<> ensureDirectory( const fs::path& dir, bool owner_only )
	{
		std::error_code ec;
		fs::create_directories( dir, ec );
		if ( ec )
		{
			return fail( "cannot create {}: {}", dir.string(), ec.message() );
		}
		if ( owner_only )
		{
			fs::permissions( dir, fs::perms::owner_all, fs::perm_options::replace, ec );
			if ( ec )
			{
				return fail( "cannot restrict {}: {}", dir.string(), ec.message() );
			}
		}
		return {};
	}

} // namespace lexiglance::paths
