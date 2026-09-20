#include <lexiglance/core/Uninstall.h>

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <system_error>

#ifndef _WIN32
	#include <fcntl.h>
	#include <unistd.h>
#endif

namespace lexiglance::uninstall
{

	namespace
	{

		namespace fs = std::filesystem;

		fs::path environment( const char* name )
		{
			const char* value = std::getenv( name );
			return value != nullptr && *value != '\0' ? fs::path( value ) : fs::path();
		}

		// An XDG base directory: the variable when it is an absolute path, else its default below the home directory.
		fs::path baseDirectory( const char* variable, const fs::path& home, std::string_view fallback )
		{
			const fs::path base = environment( variable );
			return base.empty() || base.is_relative() ? home / fallback : base;
		}

		// Whether something is there, a link that leads nowhere included.
		bool present( const fs::path& path )
		{
			std::error_code ec;
			return !path.empty() && fs::exists( fs::symlink_status( path, ec ) );
		}

		void add( std::vector<fs::path>& out, const fs::path& path )
		{
			if ( present( path ) && std::ranges::find( out, path ) == out.end() )
			{
				out.push_back( path );
			}
		}

		std::string joined( std::span<const std::string> parts, std::string_view separator )
		{
			std::string out;
			for ( const std::string& part : parts )
			{
				out.append( out.empty() ? std::string_view() : separator ).append( part );
			}
			return out;
		}

		bool isBuildTree( const fs::path& directory )
		{
			return directory.parent_path().filename() == "apps" && present( directory.parent_path().parent_path() / "CMakeCache.txt" );
		}

#ifndef _WIN32
		// Whether dpkg installed `file` as part of the lexiglance package.
		bool debOwns( const fs::path& file )
		{
			std::ifstream list( "/var/lib/dpkg/info/lexiglance.list" );
			std::string   line;
			while ( std::getline( list, line ) )
			{
				if ( line == file.string() )
				{
					return true;
				}
			}
			return false;
		}

		bool rpmOwns( const fs::path& file )
		{
			const fs::path rpm = process::findProgram( "rpm" );
			if ( rpm.empty() )
			{
				return false;
			}
			const std::array<std::string, 2> owner{ "-qf", file.string() };
			const std::array<std::string, 2> installed{ "-q", "lexiglance" };
			return process::run( rpm, owner, true ) == 0 && process::run( rpm, installed, true ) == 0;
		}

		// A service copied from share/lexiglance/services as docs/autostart.md shows, and what enabled it.
		void addServices( const Places& places, std::vector<fs::path>& out )
		{
			const fs::path config = places.config_home;
			for ( const fs::path& path : {
						  config / "systemd/user/lexiglanced.service",
						  config / "systemd/user/graphical-session.target.wants/lexiglanced.service",
						  places.home / ".local/service/lexiglanced",
						  config / "service/lexiglanced",
						  config / "rc/init.d/lexiglanced",
						  config / "rc/conf.d/lexiglanced",
						  config / "rc/runlevels/default/lexiglanced",
						  config / "dinit.d/lexiglanced",
						  config / "dinit.d/boot.d/lexiglanced",
				  } )
			{
				add( out, path );
			}
		}

		// runit brings a service back when it ends: told to take it down, and to stop watching it.
		void downRunit( const fs::path& service )
		{
			const fs::path control = service / "supervise/control";
			const int      fifo    = ::open( control.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC ); // NOLINT(cppcoreguidelines-pro-type-vararg): open(2) is variadic
			if ( fifo >= 0 )
			{
				[[maybe_unused]] const auto written = ::write( fifo, "dx", 2 );
				::close( fifo );
			}
		}

		void quietly( std::string_view name, std::initializer_list<std::string> arguments )
		{
			if ( const fs::path program = process::findProgram( name ); !program.empty() )
			{
				const std::vector<std::string> list( arguments );
				( void )process::run( program, list, true );
			}
		}
#endif

	} // namespace

	Places Places::current()
	{
		Places places;
		places.executable = process::executable();
		places.appimage   = environment( "APPIMAGE" );
#ifdef _WIN32
		places.home = environment( "USERPROFILE" );
#else
		places.home = environment( "HOME" );
#endif
		places.config_home = baseDirectory( "XDG_CONFIG_HOME", places.home, ".config" );
		places.data_home   = baseDirectory( "XDG_DATA_HOME", places.home, ".local/share" );
		places.data        = { paths::configDir(), paths::dataDir(), paths::cacheDir(), paths::stateDir() };
		return places;
	}

	Plan plan( const Places& places, bool keep_data )
	{
		Plan           out;
		const fs::path directory = places.executable.parent_path();
		const fs::path prefix    = directory.parent_path();
		if ( isBuildTree( directory ) )
		{
			out.kind  = Kind::BuildTree;
			out.where = prefix.parent_path();
		}
#ifdef _WIN32
		else if ( present( prefix / "Uninstall.exe" ) )
		{
			out.kind  = Kind::WindowsSetup;
			out.where = prefix / "Uninstall.exe";
		}
		else
		{
			// The folders the .zip brings, not the folder it was unpacked into, which can be anyone's.
			out.kind  = Kind::WindowsPortable;
			out.where = prefix;
			for ( const char* part : { "bin", "lib", "share" } )
			{
				add( out.programs, prefix / part );
			}
		}
#else
		else if ( !places.appimage.empty() )
		{
			out.kind  = Kind::AppImage;
			out.where = places.appimage;
			add( out.programs, places.appimage );
			// An update that did not finish.
			add( out.programs, fs::path( places.appimage.string() + ".update" ) );
		}
		else if ( debOwns( places.executable ) )
		{
			out.kind    = Kind::Deb;
			out.where   = prefix;
			out.package = process::findProgram( "apt-get" ).empty() ? std::vector<std::string>{ "dpkg", "--remove", "lexiglance" } : std::vector<std::string>{ "apt-get", "remove", "-y", "lexiglance" };
		}
		// An .rpm installs into /usr itself (/usr/bin/lexiglanced), so the prefix is "/usr" with nothing after it.
		else if ( ( prefix == "/usr" || prefix.string().starts_with( "/usr/" ) ) && rpmOwns( places.executable ) )
		{
			out.kind    = Kind::Rpm;
			out.where   = prefix;
			out.package = { "rpm", "--erase", "lexiglance" };
		}
		else
		{
			out.kind = Kind::Prefix;
			// What `cmake --install` puts in a prefix (the .tar.gz holds the same); only the programs themselves when
			// they are not in one.
			const bool in_prefix = directory.filename() == "bin";
			out.where            = in_prefix ? prefix : directory;
			for ( const char* program : { "lexiglance", "lexiglanced", "lexiglancectl" } )
			{
				add( out.programs, directory / program );
			}
			if ( in_prefix )
			{
				for ( const char* part : {
							  "share/lexiglance",
							  "share/applications/io.github.mattfor.lexiglance.desktop",
							  "share/icons/hicolor/scalable/apps/lexiglance.svg",
							  "share/doc/lexiglance",
							  "lib/systemd/user/lexiglanced.service",
					  } )
				{
					add( out.programs, prefix / part );
				}
			}
		}

		const fs::path config = places.config_home;
		add( out.integration, config / "autostart/lexiglance-daemon.desktop" );
		add( out.integration, config / "autostart/lexiglance-tray.desktop" );
		add( out.integration, places.data_home / "applications/io.github.mattfor.lexiglance.desktop" );
		add( out.integration, places.data_home / "icons/hicolor/scalable/apps/lexiglance.svg" );
		addServices( places, out.integration );
		// A prefix of the user's (~/.local) has its menu entry and icon where the user's own are.
		std::erase_if( out.integration, [&]( const fs::path& path ) { return std::ranges::contains( out.programs, path ); } );
#endif

		if ( !keep_data )
		{
			for ( const fs::path& path : places.data )
			{
				add( out.data, path );
			}
		}
		return out;
	}

	std::vector<std::string> describe( const Plan& plan )
	{
		std::vector<std::string> lines;
		const std::string        where = plan.where.string();
		switch ( plan.kind )
		{
			case Kind::WindowsSetup:
				lines.push_back( std::format( "Lexiglance, by its uninstaller {}", where ) );
				break;
			case Kind::WindowsPortable:
				if ( !plan.programs.empty() )
				{
					lines.push_back( std::format( "Lexiglance's folders in {}", where ) );
				}
				break;
			case Kind::AppImage:
				if ( !plan.programs.empty() )
				{
					lines.push_back( std::format( "The AppImage {}", where ) );
				}
				break;
			case Kind::Deb:
			case Kind::Rpm:
				lines.push_back( std::format( "The lexiglance package (as root: {})", joined( plan.package, " " ) ) );
				break;
			case Kind::Prefix:
				if ( !plan.programs.empty() )
				{
					const bool root = std::ranges::any_of( plan.programs, needsRoot );
					lines.push_back( std::format( "Lexiglance's programs and files in {}{}", where, root ? " (as root)" : "" ) );
				}
				break;
			case Kind::BuildTree:
				lines.push_back( std::format( "Not the build in {}, which stays", where ) );
				break;
		}
		if ( !plan.integration.empty() )
		{
			lines.emplace_back( "Lexiglance's autostart, menu entry and icon" );
		}
		if ( plan.data.empty() )
		{
			lines.emplace_back( "Not your settings, dictionaries and downloaded models, which are kept" );
		}
		else
		{
			std::vector<std::string> folders;
			std::ranges::transform( plan.data, std::back_inserter( folders ), []( const fs::path& path ) { return path.string(); } );
			lines.push_back( std::format( "Your settings, dictionaries, downloaded models, logs and cache: {}", joined( folders, ", " ) ) );
		}
		return lines;
	}

	bool needsRoot( const fs::path& path )
	{
#ifdef _WIN32
		( void )path;
		return false;
#else
		const fs::path parent = path.parent_path();
		return !parent.empty() && ::access( parent.c_str(), W_OK ) != 0;
#endif
	}

	void stopPrograms( [[maybe_unused]] const Places& places, std::chrono::milliseconds grace )
	{
#ifndef _WIN32
		// First whatever would bring the daemon back.
		if ( !process::findProgram( "systemctl" ).empty() )
		{
			quietly( "systemctl", { "--user", "disable", "--now", "lexiglanced.service" } );
		}
		for ( const fs::path& service : { places.home / ".local/service/lexiglanced", places.config_home / "service/lexiglanced" } )
		{
			if ( present( service ) )
			{
				downRunit( service );
			}
		}
		if ( present( places.config_home / "dinit.d/lexiglanced" ) )
		{
			quietly( "dinitctl", { "stop", "lexiglanced" } );
		}
		if ( present( places.config_home / "rc/init.d/lexiglanced" ) )
		{
			quietly( "rc-service", { "--user", "lexiglanced", "stop" } );
		}
#endif
		for ( const std::string_view name : { "lexiglanced", "lexiglance" } )
		{
			for ( const int pid : process::othersNamed( name ) )
			{
				( void )process::terminate( pid, name, grace );
			}
		}
	}

	std::vector<std::string> remove( std::span<const fs::path> paths )
	{
		std::vector<std::string> failures;
		for ( const fs::path& path : paths )
		{
			std::error_code ec;
			fs::remove_all( path, ec );
			if ( ec )
			{
				failures.push_back( std::format( "{}: {}", path.string(), ec.message() ) );
				continue;
			}
			// %APPDATA%\Lexiglance and %LOCALAPPDATA%\Lexiglance once their config, data and cache are gone.
			if ( const fs::path parent = path.parent_path(); parent.filename() == "Lexiglance" && fs::is_empty( parent, ec ) )
			{
				fs::remove( parent, ec );
			}
		}
		return failures;
	}

} // namespace lexiglance::uninstall
