add_executable(
    lexiglanced
    main.cpp
    Anki.cpp
    Audio.cpp
    Daemon.cpp
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

install(TARGETS lexiglanced RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
