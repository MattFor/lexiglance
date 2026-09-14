if(LEXIGLANCE_BUILD_DAEMON)
    add_subdirectory(daemon)
endif()

if(LEXIGLANCE_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(LEXIGLANCE_BUILD_CTL)
    add_subdirectory(ctl)
endif()
