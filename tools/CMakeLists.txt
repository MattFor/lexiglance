# Developer tools, not installed. ocrbench is the driver of the OCR benchmark
# (tools/ocr-bench/run.sh): the daemon's own screen reading, pointed at pictures.
add_executable(ocrbench ocr-bench/bench.cpp)
target_include_directories(ocrbench PRIVATE ${PROJECT_SOURCE_DIR}/src/platform)
target_link_libraries(ocrbench PRIVATE lexiglance::platform lexiglance::ocr
                                       lexiglance::options)
