add_executable(
    lexiglanced
    main.cpp
    Anki.cpp
    Audio.cpp
    Daemon.cpp
    Debugging.cpp
    Developer.cpp
    Health.cpp
    IpcServer.cpp
    Stats.cpp
    Translation.cpp)

target_link_libraries(
    lexiglanced PRIVATE lexiglance::platform lexiglance::net
                        lexiglance::translate lexiglance::options)

if(WIN32)
    # A background program: no console window opens when it starts (its output
    # goes to the terminal it was started from, if any). MCI plays
    # pronunciations. Run as administrator goes through a scheduled task (the
    # Task Scheduler's COM interface).
    set_target_properties(lexiglanced PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(lexiglanced PRIVATE Elevation.cpp)
    target_link_libraries(lexiglanced PRIVATE winmm ole32 oleaut32 taskschd
                                              advapi32)
endif()

if(APPLE)
    # A helper app of its own inside Lexiglance.app: in Contents/MacOS,
    # LaunchServices would take it for Lexiglance.app itself.
    set(LEXIGLANCE_DAEMON_APP Lexiglance.app/Contents/Helpers/lexiglanced.app)
    install(TARGETS lexiglanced
            RUNTIME DESTINATION ${LEXIGLANCE_DAEMON_APP}/Contents/MacOS)
    configure_file(${PROJECT_SOURCE_DIR}/data/macos/Daemon-Info.plist.in
                   Info.plist @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
            DESTINATION ${LEXIGLANCE_DAEMON_APP}/Contents)
    install(FILES ${PROJECT_SOURCE_DIR}/data/macos/lexiglance.icns
            DESTINATION ${LEXIGLANCE_DAEMON_APP}/Contents/Resources)
else()
    install(TARGETS lexiglanced RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
