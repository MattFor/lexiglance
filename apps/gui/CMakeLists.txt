find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)

add_executable(
    lexiglance
    main.cpp
    AboutPage.cpp
    AnkiPage.cpp
    AppearancePage.cpp
    CardOverlay.cpp
    Common.cpp
    DaemonClient.cpp
    DesktopEntry.cpp
    DictionariesPage.cpp
    Downloader.cpp
    HelpOverlay.cpp
    MainWindow.cpp
    OcrGroup.cpp
    OcrInstall.cpp
    OverviewPage.cpp
    ScanningPage.cpp
    SearchPage.cpp
    Settings.cpp
    SetupWizard.cpp
    StatisticsCharts.cpp
    StatisticsPage.cpp
    Themes.cpp
    TranslationInstall.cpp
    TranslationPage.cpp
    UninstallOverlay.cpp
    UpdateGroup.cpp
    VcRedist.cpp
    resources.qrc)
set_target_properties(lexiglance PROPERTIES AUTORCC ON)

# The window's icons: Breeze (KDE, LGPL 3.0) as an icon theme of its own, light
# and dark, the same on every desktop.
file(GLOB_RECURSE LEXIGLANCE_ICON_THEME
     ${PROJECT_SOURCE_DIR}/data/icons/theme/*/index.theme
     ${PROJECT_SOURCE_DIR}/data/icons/theme/*.png)
qt_add_resources(
    lexiglance
    "icon-theme"
    PREFIX
    "/icons"
    BASE
    ${PROJECT_SOURCE_DIR}/data/icons/theme
    FILES
    ${LEXIGLANCE_ICON_THEME})

# lexiglance::ocr for the Visual C++ runtime check behind Health's Fix issues
# and the PaddleOCR download (VcRedist.cpp); lexiglance::translate for what a
# translation model consists of.
target_link_libraries(
    lexiglance
    PRIVATE lexiglance::core
            lexiglance::ocr
            lexiglance::translate
            lexiglance::options
            Qt6::Widgets
            Qt6::Network)

install(TARGETS lexiglance RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    # A windowed program: no console opens with it.
    set_target_properties(lexiglance PROPERTIES WIN32_EXECUTABLE ON)
    # Qt's plugins (the Windows platform, styles, image formats, TLS) next to
    # the installed program.
    qt_generate_deploy_app_script(
        TARGET
        lexiglance
        OUTPUT_SCRIPT
        LEXIGLANCE_QT_DEPLOY
        NO_TRANSLATIONS
        NO_COMPILER_RUNTIME
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${LEXIGLANCE_QT_DEPLOY})
endif()
