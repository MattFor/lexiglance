# Which build this is, shown in About and copied into bug reports: "stable" for
# one made from a release tag (the release workflow, or a checkout of v1.2.3),
# "dev" for everything else - a build from master, from a work tree, or from a
# source archive with no history. The commit comes along when git knows it.
#
# Both are worked out when CMake configures, so a build from a later commit
# keeps the commit it was configured at until the next configure.

set(LEXIGLANCE_CHANNEL_DEFAULT "dev")
set(LEXIGLANCE_COMMIT_DEFAULT "")

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE LEXIGLANCE_COMMIT_DEFAULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # Exactly this version's tag, with nothing changed on top of it.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --dirty
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        RESULT_VARIABLE LEXIGLANCE_TAG_FOUND
        OUTPUT_VARIABLE LEXIGLANCE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(LEXIGLANCE_TAG_FOUND EQUAL 0 AND LEXIGLANCE_TAG STREQUAL
                                        "v${PROJECT_VERSION}")
        set(LEXIGLANCE_CHANNEL_DEFAULT "stable")
    endif()
endif()
# The release workflow builds a pushed tag, where the tag itself may not have
# been fetched; its word settles it.
if(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(LEXIGLANCE_CHANNEL_DEFAULT "stable")
endif()

set(LEXIGLANCE_CHANNEL
    "${LEXIGLANCE_CHANNEL_DEFAULT}"
    CACHE STRING "Build channel shown in About: stable or dev")
set_property(CACHE LEXIGLANCE_CHANNEL PROPERTY STRINGS stable dev)
set(LEXIGLANCE_COMMIT
    "${LEXIGLANCE_COMMIT_DEFAULT}"
    CACHE STRING "Commit shown in About; empty when it is not known")
