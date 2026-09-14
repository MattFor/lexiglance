# Shared compiler configuration. Every first-party target links
# `lexiglance::options` privately so that warnings and optimisation flags apply
# to our code only and never leak into third-party headers.

add_library(lexiglance_options INTERFACE)
add_library(lexiglance::options ALIAS lexiglance_options)

# A baseline CPU for distributed builds, e.g. x86-64-v2 (every x86-64 CPU since
# about 2009); empty keeps the compiler's.
set(LEXIGLANCE_ARCH
    ""
    CACHE STRING "Target CPU level (-march) for distributed builds")
# Profile-guided optimisation: build with GENERATE, run a representative
# workload, build again with USE. With Clang, merge the profiles into
# merged.profdata (llvm-profdata merge) before the second build.
set(LEXIGLANCE_PGO
    "OFF"
    CACHE STRING "Profile-guided optimisation: OFF, GENERATE or USE")
set_property(CACHE LEXIGLANCE_PGO PROPERTY STRINGS OFF GENERATE USE)
set(LEXIGLANCE_PGO_DIR
    "${PROJECT_BINARY_DIR}/pgo"
    CACHE PATH "Where profiles are written and read")

if(MSVC)
    target_compile_options(
        lexiglance_options
        INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
                  $<$<BOOL:${LEXIGLANCE_WARNINGS_AS_ERRORS}>:/WX>)
    target_compile_definitions(
        lexiglance_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE
                                     _UNICODE)
else()
    target_compile_options(
        lexiglance_options
        INTERFACE -Wall
                  -Wextra
                  -Wpedantic
                  -Wshadow
                  -Wnon-virtual-dtor
                  -Woverloaded-virtual
                  -Wcast-align
                  -Wimplicit-fallthrough
                  -Wformat=2
                  -Wmissing-declarations
                  -Wno-missing-field-initializers
                  $<$<BOOL:${LEXIGLANCE_WARNINGS_AS_ERRORS}>:-Werror>
                  $<$<BOOL:${LEXIGLANCE_NATIVE}>:-march=native>)

    if(LEXIGLANCE_ARCH AND NOT LEXIGLANCE_NATIVE)
        target_compile_options(lexiglance_options
                               INTERFACE -march=${LEXIGLANCE_ARCH})
    endif()
    if(NOT APPLE)
        # Calls into shared libraries go straight through the GOT instead of a
        # PLT stub.
        target_compile_options(
            lexiglance_options
            INTERFACE $<$<CONFIG:Release,RelWithDebInfo>:-fno-plt>)
    endif()

    if(LEXIGLANCE_PGO STREQUAL "GENERATE")
        target_compile_options(
            lexiglance_options
            INTERFACE -fprofile-generate=${LEXIGLANCE_PGO_DIR})
        target_link_options(lexiglance_options INTERFACE
                            -fprofile-generate=${LEXIGLANCE_PGO_DIR})
    elseif(LEXIGLANCE_PGO STREQUAL "USE" AND CMAKE_CXX_COMPILER_ID STREQUAL
                                             "Clang")
        target_compile_options(
            lexiglance_options
            INTERFACE -fprofile-use=${LEXIGLANCE_PGO_DIR}/merged.profdata
                      -Wno-profile-instr-unprofiled)
    elseif(LEXIGLANCE_PGO STREQUAL "USE")
        target_compile_options(
            lexiglance_options
            INTERFACE -fprofile-use=${LEXIGLANCE_PGO_DIR}
                      -fprofile-partial-training -fprofile-correction
                      -Wno-missing-profile)
    endif()

    if(LEXIGLANCE_FUZZ)
        # Coverage feedback for libFuzzer in every library the fuzzers reach.
        target_compile_options(lexiglance_options
                               INTERFACE -fsanitize=fuzzer-no-link)
    endif()

    if(LEXIGLANCE_SANITIZE)
        target_compile_options(
            lexiglance_options INTERFACE -fsanitize=address,undefined
                                         -fno-omit-frame-pointer)
        target_link_options(lexiglance_options INTERFACE
                            -fsanitize=address,undefined)
    endif()
endif()

if(MINGW)
    # Static executables need no MinGW runtime DLLs next to them: the default
    # for the portable part cross-built on Linux. A native build of the daemon
    # and the settings application uses the DLLs of Cairo, Pango and Qt anyway,
    # so the runtime comes along as DLLs too (cmake --install puts them next to
    # the executables).
    if(CMAKE_CROSSCOMPILING)
        set(LEXIGLANCE_STATIC_DEFAULT ON)
    else()
        set(LEXIGLANCE_STATIC_DEFAULT OFF)
    endif()
    option(LEXIGLANCE_STATIC "Link the executables statically (MinGW)"
           ${LEXIGLANCE_STATIC_DEFAULT})

    # MinGW's libstdc++ keeps std::print's terminal support in a separate
    # library.
    target_link_libraries(lexiglance_options INTERFACE stdc++exp)
    if(LEXIGLANCE_STATIC)
        target_link_options(lexiglance_options INTERFACE -static)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION
                                                    VERSION_LESS 15)
            # GCC 14 defines type_info::operator== both inline (C++23) and in
            # the static libstdc++ (GCC bug 110572); the two definitions are the
            # same function.
            target_link_options(lexiglance_options INTERFACE
                                -Wl,--allow-multiple-definition)
        endif()
    endif()
endif()

if(LEXIGLANCE_ENABLE_LTO AND NOT LEXIGLANCE_SANITIZE)
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT LEXIGLANCE_IPO_SUPPORTED
        OUTPUT LEXIGLANCE_IPO_OUTPUT
        LANGUAGES CXX)

    if(LEXIGLANCE_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(
            STATUS
                "Link-time optimisation is not supported: ${LEXIGLANCE_IPO_OUTPUT}"
        )
    endif()
endif()
