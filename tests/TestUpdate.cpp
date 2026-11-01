#include "Test.h"

#include <lexiglance/core/Paths.h>
#include <lexiglance/core/Process.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace
{

	namespace fs   = std::filesystem;
	namespace lg   = lexiglance;
	namespace test = lexiglance::test;

	void touch( const fs::path& path )
	{
		fs::create_directories( path.parent_path() );
		std::ofstream( path ) << "x";
	}

	// AppImages are Linux's; the folders differ only in case, which other systems' file systems ignore.
#ifdef __linux__
	const test::Registrar appimage_folder( "update: the AppImage goes where the user keeps AppImages, in whatever case", [] {
		const fs::path home = test::scratch( "appimage-folder" );
		test::expectEqual( lg::paths::appImageFolder( home ), home / "Applications" );
		fs::create_directories( home / "apps" );
		touch( home / "AppImages/Other.AppImage" );
		test::expectEqual( lg::paths::appImageFolder( home ), home / "AppImages" );
		// A folder holding only Lexiglance gives way to the one with the user's own files.
		touch( home / "Applications/lexiglance-x86_64.AppImage" );
		touch( home / "applications/a.AppImage" );
		touch( home / "applications/b.AppImage" );
		test::expectEqual( lg::paths::appImageFolder( home ), home / "applications" );
	} );

	const test::Registrar appimage_destination( "update: an AppImage alone in a folder of its own moves beside the user's others", [] {
		const fs::path home = test::scratch( "appimage-destination" );
		const fs::path ours = home / "Applications/lexiglance-x86_64.AppImage";
		touch( ours );
		// No folder of the user's to move to.
		test::expect( lg::paths::appImageDestination( ours, home ).empty() );
		touch( home / "applications/Other.AppImage" );
		test::expectEqual( lg::paths::appImageDestination( ours, home ), home / "applications/lexiglance-x86_64.AppImage" );
		// Nor elsewhere than in such a folder directly in the home: a download stays where it is.
		touch( home / "downloads/lexiglance-x86_64.AppImage" );
		test::expect( lg::paths::appImageDestination( home / "downloads/lexiglance-x86_64.AppImage", home ).empty() );
		// Nor when the folder holds the user's files too, or a copy is there already.
		touch( home / "Applications/Mine.AppImage" );
		test::expect( lg::paths::appImageDestination( ours, home ).empty() );
		fs::remove( home / "Applications/Mine.AppImage" );
		touch( home / "applications/lexiglance-x86_64.AppImage" );
		test::expect( lg::paths::appImageDestination( ours, home ).empty() );
	} );

	const test::Registrar replacement( "update: a copy replaced while it runs is taken over once the new file is whole", [] {
		test::expectEqual( lg::process::programFile(), lg::process::executable() );
		const fs::path appimage = test::scratch( "replacement" ) / "lexiglance-x86_64.AppImage";
		::setenv( "APPIMAGE", appimage.c_str(), 1 );
		test::expectEqual( lg::process::programFile(), appimage );
		std::optional<fs::file_time_type> seen;
		test::expect( !lg::process::replacementReady( seen ) );

		// A newer file in its place (written after this process started), executable: ready at the second look.
		touch( appimage );
		fs::permissions( appimage, fs::perms::owner_all );
		fs::last_write_time( appimage, fs::file_time_type::clock::now() + std::chrono::hours( 1 ) );
		test::expect( lg::process::executableReplaced() );
		test::expect( !lg::process::replacementReady( seen ) );
		test::expect( lg::process::replacementReady( seen ) );

		// Still being written between two looks: not yet.
		seen.reset();
		test::expect( !lg::process::replacementReady( seen ) );
		fs::last_write_time( appimage, fs::file_time_type::clock::now() + std::chrono::hours( 2 ) );
		test::expect( !lg::process::replacementReady( seen ) );
		test::expect( lg::process::replacementReady( seen ) );

		// Not executable (a download nobody made runnable): never.
		fs::permissions( appimage, fs::perms::owner_read | fs::perms::owner_write );
		test::expect( !lg::process::replacementReady( seen ) );
		test::expect( !lg::process::replacementReady( seen ) );

		::unsetenv( "APPIMAGE" );
		test::expect( !lg::process::replacementReady( seen ) );
	} );

	const test::Registrar foreign_appimage( "update: APPIMAGE inherited from another AppImage's shell is forgotten", [] {
		// The mount named as given, not canonical.
		::setenv( "APPDIR", ( lg::process::executable().parent_path().string() + "//." ).c_str(), 1 );
		::setenv( "APPIMAGE", "/opt/Other.AppImage", 1 );
		lg::process::forgetForeignAppImage();
		test::expect( std::getenv( "APPIMAGE" ) != nullptr );
		::setenv( "APPDIR", test::scratch( "foreign-appimage" ).c_str(), 1 );
		lg::process::forgetForeignAppImage();
		test::expect( std::getenv( "APPIMAGE" ) == nullptr && std::getenv( "APPDIR" ) == nullptr );
	} );
#endif

} // namespace
