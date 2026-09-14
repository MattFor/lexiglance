# 3.30: the first CMake that knows GCC's C++26 mode.
cmake_minimum_required(VERSION 3.30)

project(
    lexiglance
    VERSION 1.0.0
    DESCRIPTION
        "System-wide pop-up dictionary compatible with Yomitan dictionaries"
    HOMEPAGE_URL "https://github.com/MattFor/lexiglance"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules are used; scanning only adds GCC flags that clang based tools
# reject.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------------------------------------
option(LEXIGLANCE_BUILD_DAEMON "Build the lookup daemon (lexiglanced)" ON)
option(LEXIGLANCE_BUILD_GUI "Build the Qt settings application (lexiglance)" ON)
option(LEXIGLANCE_BUILD_CTL "Build the lexiglancectl scripting client" ON)
option(LEXIGLANCE_BUILD_TESTS "Build the unit tests" ON)
option(LEXIGLANCE_WITH_ATSPI
       "Read text under the cursor through AT-SPI (Linux)" ON)
option(LEXIGLANCE_ENABLE_LTO
       "Enable link-time optimisation for optimised builds" ON)
option(LEXIGLANCE_NATIVE
       "Optimise for the CPU of the build machine (-march=native)" OFF)
option(LEXIGLANCE_SANITIZE
       "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LEXIGLANCE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(
    LEXIGLANCE_FUZZ
    "Build the libFuzzer targets in tests/fuzz (Clang; combine with LEXIGLANCE_SANITIZE)"
    OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(GNUInstallDirs)

find_package(Threads REQUIRED)
# A static build must not pick the DLL's import library, or its executables need
# zlib1.dll next to them.
if(LEXIGLANCE_STATIC)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------------------------------------------------
# Platform selection
# ---------------------------------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(LEXIGLANCE_PLATFORM
        "x11"
        CACHE STRING "Desktop integration backend used by the daemon")
elseif(WIN32)
    set(LEXIGLANCE_PLATFORM
        "windows"
        CACHE STRING "Desktop integration backend used by the daemon")
else()
    set(LEXIGLANCE_PLATFORM
        "null"
        CACHE STRING "Desktop integration backend used by the daemon")
endif()
set_property(CACHE LEXIGLANCE_PLATFORM PROPERTY STRINGS x11 windows null)

if(WIN32)
    enable_language(RC)
endif()

add_subdirectory(src)
add_subdirectory(apps)

if(LEXIGLANCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(LEXIGLANCE_FUZZ)
    add_subdirectory(tests/fuzz)
endif()

# Windows executables carry a manifest that makes UTF-8 their code page.
if(WIN32)
    configure_file(data/windows/lexiglance.rc.in lexiglance.rc @ONLY)
    foreach(executable lexiglancectl lexiglanced lexiglance lexiglance_tests)
        if(TARGET ${executable})
            target_sources(${executable}
                           PRIVATE ${PROJECT_BINARY_DIR}/lexiglance.rc)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Installation of desktop integration files
# ---------------------------------------------------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    install(FILES data/linux/io.github.mattfor.lexiglance.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/icons/lexiglance.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

    # Service definitions for starting the daemon (docs/autostart.md), with its
    # installed path filled in.
    set(LEXIGLANCE_DAEMON "${CMAKE_INSTALL_FULL_BINDIR}/lexiglanced")
    foreach(service systemd/lexiglanced.service dinit/lexiglanced
                    s6/lexiglanced/type xdg/lexiglance-daemon.desktop)
        configure_file(data/services/${service}.in services/${service} @ONLY)
    endforeach()
    foreach(script runit/lexiglanced/run openrc/lexiglanced s6/lexiglanced/run)
        configure_file(
            data/services/${script}.in
            services/${script}
            @ONLY
            FILE_PERMISSIONS
            OWNER_READ
            OWNER_WRITE
            OWNER_EXECUTE
            GROUP_READ
            GROUP_EXECUTE
            WORLD_READ
            WORLD_EXECUTE)
    endforeach()
    install(
        DIRECTORY ${PROJECT_BINARY_DIR}/services/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/lexiglance/services
        USE_SOURCE_PERMISSIONS)
    install(FILES ${PROJECT_BINARY_DIR}/services/systemd/lexiglanced.service
            DESTINATION lib/systemd/user)
endif()
if(WIN32 AND NOT LEXIGLANCE_STATIC)
    # CMake 4: runtime dependencies come with normalised paths (no notice per
    # system DLL).
    if(POLICY CMP0207)
        cmake_policy(SET CMP0207 NEW)
    endif()
    # The DLLs the executables load (Cairo, Pango, libcurl, Qt, the MinGW
    # runtime) go next to them; Windows' own stay.
    get_filename_component(LEXIGLANCE_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}"
                           DIRECTORY)
    set(LEXIGLANCE_PROGRAMS)
    foreach(program lexiglancectl lexiglanced lexiglance)
        if(TARGET ${program})
            list(APPEND LEXIGLANCE_PROGRAMS ${program})
        endif()
    endforeach()
    install(
        TARGETS ${LEXIGLANCE_PROGRAMS}
                RUNTIME_DEPENDENCIES
                PRE_EXCLUDE_REGEXES
                "^api-ms-"
                "^ext-ms-"
                POST_EXCLUDE_REGEXES
                ".*[/\\\\][Ss]ystem32[/\\\\].*"
                ".*[/\\\\][Ww]indows[/\\\\].*"
                DIRECTORIES
                "${LEXIGLANCE_TOOLCHAIN_BIN}"
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------------------------------------------------
# Packages (cpack): .tar.gz and .deb on Linux, .zip on Windows and macOS
# ---------------------------------------------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME lexiglance)
set(CPACK_PACKAGE_VENDOR MattFor)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES ON)
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
               LEXIGLANCE_PACKAGE_SYSTEM)
set(CPACK_PACKAGE_FILE_NAME
    "lexiglance-${PROJECT_VERSION}-${LEXIGLANCE_PACKAGE_SYSTEM}")
if(WIN32 OR APPLE)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ DEB)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER
        "MattFor <MattFor@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SUGGESTS
        "tesseract-ocr-jpn, ffmpeg | mpv | mpg123")
endif()
include(CPack)

message(STATUS "")
message(STATUS "Lexiglance ${PROJECT_VERSION}")
message(STATUS "  Build type ........... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform backend ..... ${LEXIGLANCE_PLATFORM}")
message(
    STATUS
        "  Daemon / GUI / CTL ... ${LEXIGLANCE_BUILD_DAEMON} / ${LEXIGLANCE_BUILD_GUI} / ${LEXIGLANCE_BUILD_CTL}"
)
message(STATUS "  AT-SPI ............... ${LEXIGLANCE_HAVE_ATSPI}")
message(STATUS "  Tests ................ ${LEXIGLANCE_BUILD_TESTS}")
message(
    STATUS "  CPU / PGO ............ ${LEXIGLANCE_ARCH} / ${LEXIGLANCE_PGO}")
message(STATUS "")
