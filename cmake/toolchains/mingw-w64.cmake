# Cross-compiling for Windows with MinGW-w64 (the portable part: core,
# lexiglancectl, tests), for example:
#
#   cmake -S . -B build/windows -G Ninja \
#         --toolchain cmake/toolchains/mingw-w64.cmake \
#         -DLEXIGLANCE_BUILD_DAEMON=OFF -DLEXIGLANCE_BUILD_GUI=OFF
#
# A static zlib for MinGW is needed (Fedora: mingw64-zlib-static; elsewhere
# pass -DZLIB_INCLUDE_DIR and -DZLIB_LIBRARY). The executables are linked
# statically, and ctest runs the Windows test binary through wine.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Debian, Arch and Void keep the MinGW sysroot in the first place, Fedora in the
# second.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32
                         /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_CROSSCOMPILING_EMULATOR wine)
