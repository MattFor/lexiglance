add_library(lexiglance_platform STATIC Platform.cpp Capture.cpp
                                       ocr/OcrCapture.cpp ocr/Tesseract.cpp)
add_library(lexiglance::platform ALIAS lexiglance_platform)

target_include_directories(lexiglance_platform
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
    lexiglance_platform
    PUBLIC lexiglance::render
    PRIVATE lexiglance::ocr lexiglance::options ${CMAKE_DL_LIBS})

set(LEXIGLANCE_HAVE_ATSPI
    OFF
    CACHE INTERNAL "")

if(LEXIGLANCE_PLATFORM STREQUAL "x11")
    pkg_check_modules(
        X11DEPS
        REQUIRED
        IMPORTED_TARGET
        x11
        xi
        xfixes
        xext
        xrandr
        cairo-xlib)

    target_sources(lexiglance_platform PRIVATE linux/X11Backend.cpp
                                               linux/X11Screen.cpp)
    target_link_libraries(lexiglance_platform PRIVATE PkgConfig::X11DEPS)
    target_compile_definitions(lexiglance_platform
                               PRIVATE LEXIGLANCE_PLATFORM_X11=1)

    if(LEXIGLANCE_WITH_ATSPI)
        pkg_check_modules(ATSPI IMPORTED_TARGET atspi-2 gio-2.0)
        if(ATSPI_FOUND)
            target_sources(lexiglance_platform PRIVATE linux/AtspiCapture.cpp)
            target_link_libraries(lexiglance_platform PRIVATE PkgConfig::ATSPI)
            target_compile_definitions(lexiglance_platform
                                       PRIVATE LEXIGLANCE_HAVE_ATSPI=1)
            set(LEXIGLANCE_HAVE_ATSPI
                ON
                CACHE INTERNAL "")
        else()
            message(
                WARNING
                    "AT-SPI not found: text under the cursor cannot be read, only selections"
            )
        endif()
    endif()
elseif(LEXIGLANCE_PLATFORM STREQUAL "windows")
    # Raw Input, layered windows, UI Automation and GDI screen reads; all part
    # of Windows itself.
    target_sources(
        lexiglance_platform
        PRIVATE windows/WindowsBackend.cpp windows/UiaCapture.cpp
                windows/GdiScreen.cpp)
    target_link_libraries(
        lexiglance_platform
        PRIVATE user32
                gdi32
                ole32
                oleaut32
                uuid
                dwmapi
                shcore)
    target_compile_definitions(lexiglance_platform
                               PRIVATE LEXIGLANCE_PLATFORM_WINDOWS=1)
endif()
