#include "Test.h"

#include <lexiglance/core/Uninstall.h>

#include <algorithm>
#include <fstream>

namespace
{

	namespace fs   = std::filesystem;
	namespace test = lexiglance::test;
	namespace un   = lexiglance::uninstall;

	void touch( const fs::path& path )
	{
		fs::create_directories( path.parent_path() );
		std::ofstream( path ) << "x";
	}

	bool has( const std::vector<fs::path>& paths, const fs::path& path )
	{
		return std::ranges::contains( paths, path );
	}

	// A user's home with Lexiglance's autostart entry, and its settings folder but no data folder yet.
	un::Places placesIn( const fs::path& root, const fs::path& executable )
	{
		un::Places places;
		places.executable  = executable;
		places.home        = root / "home";
		places.config_home = places.home / ".config";
		places.data_home   = places.home / ".local/share";
		places.data        = { places.config_home / "lexiglance", places.data_home / "lexiglance" };
		touch( places.config_home / "autostart/lexiglance-daemon.desktop" );
		fs::create_directories( places.config_home / "lexiglance" );
		return places;
	}

	const test::Registrar build_tree_plan( "uninstall plan: a build tree stays", [] {
		const fs::path root  = test::scratch( "uninstall-build" );
		const fs::path build = root / "build";
		touch( build / "CMakeCache.txt" );
		touch( build / "apps/gui/lexiglance" );
		const auto places = placesIn( root, build / "apps/gui/lexiglance" );

		const auto plan = un::plan( places, false );
		test::expect( plan.kind == un::Kind::BuildTree );
		test::expect( plan.where == build );
		test::expect( plan.programs.empty() );
		test::expect( plan.package.empty() );
#ifdef _WIN32
		// The setup's uninstaller or the settings application takes care of Windows' autostart and Start menu entry.
		test::expect( plan.integration.empty() );
#else
		test::expect( has( plan.integration, places.config_home / "autostart/lexiglance-daemon.desktop" ) );
#endif
		// Only what is there.
		test::expectEqual( plan.data.size(), std::size_t{ 1 } );
		test::expect( has( plan.data, places.config_home / "lexiglance" ) );
		test::expect( un::plan( places, true ).data.empty() );
	} );

#ifdef _WIN32
	const test::Registrar windows_plan( "uninstall plan: the Windows setup and the portable copy", [] {
		const fs::path root = test::scratch( "uninstall-windows" );
		touch( root / "portable/bin/lexiglance.exe" );
		fs::create_directories( root / "portable/share" );
		touch( root / "portable/notes.txt" );
		const auto portable = un::plan( placesIn( root, root / "portable/bin/lexiglance.exe" ), true );
		test::expect( portable.kind == un::Kind::WindowsPortable );
		test::expect( has( portable.programs, root / "portable/bin" ) );
		test::expect( has( portable.programs, root / "portable/share" ) );
		// Not the folder it was unpacked into, nor anything else in it.
		test::expectEqual( portable.programs.size(), std::size_t{ 2 } );

		touch( root / "setup/Uninstall.exe" );
		touch( root / "setup/bin/lexiglance.exe" );
		const auto setup = un::plan( placesIn( root, root / "setup/bin/lexiglance.exe" ), false );
		test::expect( setup.kind == un::Kind::WindowsSetup );
		test::expect( setup.where == root / "setup/Uninstall.exe" );
	} );
#else
	const test::Registrar prefix_plan( "uninstall plan: Lexiglance's files in a prefix, and nothing else there", [] {
		const fs::path root   = test::scratch( "uninstall-prefix" );
		const fs::path prefix = root / "prefix";
		for ( const char* file : { "bin/lexiglance", "bin/lexiglanced", "bin/lexiglancectl", "bin/other", "share/lexiglance/services/x", "share/applications/io.github.mattfor.lexiglance.desktop", "share/applications/other.desktop" } )
		{
			touch( prefix / file );
		}
		const auto plan = un::plan( placesIn( root, prefix / "bin/lexiglance" ), false );
		test::expect( plan.kind == un::Kind::Prefix );
		test::expect( plan.where == prefix );
		test::expectEqual( plan.programs.size(), std::size_t{ 5 } );
		test::expect( has( plan.programs, prefix / "bin/lexiglancectl" ) );
		test::expect( has( plan.programs, prefix / "share/lexiglance" ) );
		test::expect( has( plan.programs, prefix / "share/applications/io.github.mattfor.lexiglance.desktop" ) );
		test::expect( !has( plan.programs, prefix / "bin/other" ) );
		test::expect( !un::describe( plan ).empty() );
	} );

	const test::Registrar appimage_plan( "uninstall plan: the AppImage file", [] {
		const fs::path root = test::scratch( "uninstall-appimage" );
		touch( root / "Applications/lexiglance-x86_64.AppImage" );
		auto places     = placesIn( root, root / "mount/usr/bin/lexiglance" );
		places.appimage = root / "Applications/lexiglance-x86_64.AppImage";
		touch( places.data_home / "applications/io.github.mattfor.lexiglance.desktop" );
		const auto plan = un::plan( places, true );
		test::expect( plan.kind == un::Kind::AppImage );
		test::expect( plan.programs == std::vector<fs::path>{ places.appimage } );
		test::expectEqual( plan.integration.size(), std::size_t{ 2 } );
	} );

	const test::Registrar link_removal( "uninstall removes a link, not what it leads to", [] {
		const fs::path root = test::scratch( "uninstall-links" );
		touch( root / "elsewhere/run" );
		fs::create_directories( root / "service" );
		fs::create_directory_symlink( root / "elsewhere", root / "service/lexiglanced" );
		test::expect( un::remove( std::vector<fs::path>{ root / "service/lexiglanced" } ).empty() );
		test::expect( !fs::exists( fs::symlink_status( root / "service/lexiglanced" ) ) );
		test::expect( fs::exists( root / "elsewhere/run" ) );
	} );
#endif

	const test::Registrar folder_removal( "uninstall removes folders, and the Lexiglance folder they leave empty", [] {
		const fs::path root = test::scratch( "uninstall-remove" );
		touch( root / "Lexiglance/config/config.json" );
		touch( root / "Lexiglance/data/dictionaries/x/y" );
		touch( root / "other/keep" );
		const auto failures = un::remove( std::vector<fs::path>{ root / "Lexiglance/config" } );
		test::expect( failures.empty() );
		test::expect( fs::exists( root / "Lexiglance/data" ) );
		test::expect( un::remove( std::vector<fs::path>{ root / "Lexiglance/data" } ).empty() );
		test::expect( !fs::exists( root / "Lexiglance" ) );
		test::expect( fs::exists( root / "other/keep" ) );
	} );

} // namespace
