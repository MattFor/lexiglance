configure_file(core/Version.h.in
               ${PROJECT_BINARY_DIR}/generated/lexiglance/core/Version.h @ONLY)

# The language files in data/languages, built into the program.
file(GLOB LEXIGLANCE_LANGUAGE_FILES CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/data/languages/*.json)
list(SORT LEXIGLANCE_LANGUAGE_FILES)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${LEXIGLANCE_LANGUAGE_FILES})
set(LANGUAGE_FILES "")
foreach(language_file IN LISTS LEXIGLANCE_LANGUAGE_FILES)
    file(READ ${language_file} language_json)
    string(APPEND LANGUAGE_FILES
           "        R\"lexiglance(${language_json})lexiglance\",\n")
endforeach()
configure_file(
    language/BuiltinLanguages.h.in
    ${PROJECT_BINARY_DIR}/generated/lexiglance/language/BuiltinLanguages.h
    @ONLY)

add_library(
    lexiglance_core STATIC
    config/Config.cpp
    config/Keys.cpp
    core/Health.cpp
    core/Json.cpp
    core/Log.cpp
    core/MappedFile.cpp
    core/Md5.cpp
    core/Paths.cpp
    core/Process.cpp
    core/Thread.cpp
    core/Utf8.cpp
    core/Zip.cpp
    dictionary/Classify.cpp
    dictionary/Dictionary.cpp
    dictionary/DictionaryStore.cpp
    dictionary/Importer.cpp
    dictionary/SearchKey.cpp
    dictionary/StructuredContent.cpp
    dictionary/WordRules.cpp
    ipc/Messages.cpp
    ipc/Protocol.cpp
    ipc/Socket.cpp
    language/Alphabetic.cpp
    language/Defined.cpp
    language/Deinflector.cpp
    language/Language.cpp
    language/ja/Furigana.cpp
    language/ja/Japanese.cpp
    language/ru/Russian.cpp
    lookup/DictionarySet.cpp
    lookup/Translator.cpp)
add_library(lexiglance::core ALIAS lexiglance_core)

target_compile_definitions(lexiglance_core PRIVATE ZLIB_CONST)
if(WIN32)
    # Process owners (token SIDs) for the instance lock and the daemon's pipe.
    target_link_libraries(lexiglance_core PRIVATE advapi32)
endif()

target_include_directories(
    lexiglance_core PUBLIC ${PROJECT_SOURCE_DIR}/include
                           ${PROJECT_BINARY_DIR}/generated)

target_link_libraries(
    lexiglance_core
    PUBLIC Threads::Threads
    PRIVATE ZLIB::ZLIB lexiglance::options)

# Neural OCR (PaddleOCR through ONNX Runtime, loaded at run time).
add_library(lexiglance_ocr STATIC ocr/Availability.cpp ocr/Onnx.cpp
                                  ocr/Paddle.cpp)
add_library(lexiglance::ocr ALIAS lexiglance_ocr)
target_include_directories(
    lexiglance_ocr SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/onnxruntime)
target_link_libraries(
    lexiglance_ocr
    PUBLIC lexiglance::core
    PRIVATE lexiglance::options ${CMAKE_DL_LIBS})

# Popup rendering (Cairo + Pango) and desktop integration are only needed by the
# daemon.
if(LEXIGLANCE_BUILD_DAEMON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo cairo)

    add_library(lexiglance_render STATIC
                render/Highlight.cpp render/PopupRenderer.cpp render/Theme.cpp)
    add_library(lexiglance::render ALIAS lexiglance_render)

    target_link_libraries(
        lexiglance_render
        PUBLIC lexiglance::core PkgConfig::PANGOCAIRO
        PRIVATE lexiglance::options)

    # HTTP for audio sources and AnkiConnect.
    find_package(CURL)
    add_library(lexiglance_net STATIC net/Http.cpp)
    add_library(lexiglance::net ALIAS lexiglance_net)
    target_link_libraries(
        lexiglance_net
        PUBLIC lexiglance::core
        PRIVATE lexiglance::options)
    if(CURL_FOUND)
        target_link_libraries(lexiglance_net PRIVATE CURL::libcurl)
        target_compile_definitions(lexiglance_net
                                   PRIVATE LEXIGLANCE_HAVE_CURL=1)
    else()
        message(WARNING "libcurl not found: audio and Anki export are disabled")
    endif()

    add_subdirectory(platform)
endif()
