add_executable(lexiglancectl main.cpp)

target_link_libraries(
    lexiglancectl PRIVATE lexiglance::core lexiglance::ocr lexiglance::translate
                          lexiglance::options)

install(TARGETS lexiglancectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
