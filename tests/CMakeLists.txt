set(FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/fixture)
set(FIXTURE_ZIP ${CMAKE_CURRENT_BINARY_DIR}/fixture.zip)

file(
    GLOB FIXTURE_FILES
    RELATIVE ${FIXTURE_DIR}
    ${FIXTURE_DIR}/*)
add_custom_command(
    OUTPUT ${FIXTURE_ZIP}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${FIXTURE_ZIP} --format=zip --
            ${FIXTURE_FILES}
    WORKING_DIRECTORY ${FIXTURE_DIR}
    DEPENDS ${FIXTURE_DIR}/index.json ${FIXTURE_DIR}/term_bank_1.json
    COMMENT "Packing the fixture dictionary")
add_custom_target(lexiglance_fixture DEPENDS ${FIXTURE_ZIP})

add_executable(
    lexiglance_tests
    Test.cpp
    TestClassify.cpp
    TestCore.cpp
    TestDictionary.cpp
    TestHealth.cpp
    TestLanguage.cpp
    TestLanguageFiles.cpp
    TestOcr.cpp
    TestRussian.cpp
    TestScripts.cpp)
add_dependencies(lexiglance_tests lexiglance_fixture)

target_compile_definitions(
    lexiglance_tests PRIVATE LEXIGLANCE_TEST_DATA="${FIXTURE_DIR}"
                             LEXIGLANCE_TEST_ZIP="${FIXTURE_ZIP}")

target_link_libraries(lexiglance_tests PRIVATE lexiglance::core lexiglance::ocr
                                               lexiglance::options)

# Popup rendering and the daemon's Anki helpers, when the daemon is built.
if(LEXIGLANCE_BUILD_DAEMON)
    target_sources(
        lexiglance_tests
        PRIVATE TestRender.cpp
                TestThemes.cpp
                TestAnki.cpp
                TestIpc.cpp
                ${PROJECT_SOURCE_DIR}/apps/daemon/Anki.cpp
                ${PROJECT_SOURCE_DIR}/apps/daemon/Audio.cpp
                ${PROJECT_SOURCE_DIR}/apps/daemon/IpcServer.cpp)
    target_include_directories(lexiglance_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/apps/daemon)
    target_link_libraries(lexiglance_tests PRIVATE lexiglance::render
                                                   lexiglance::net)
    if(WIN32)
        # Audio plays through MCI.
        target_link_libraries(lexiglance_tests PRIVATE winmm)
    endif()
endif()

add_test(NAME lexiglance_tests COMMAND lexiglance_tests)
