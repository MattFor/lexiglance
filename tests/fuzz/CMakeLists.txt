# libFuzzer targets for everything that reads untrusted input. Configure with
# Clang, for example: cmake -S . -B build/fuzz -G Ninja
# -DCMAKE_CXX_COMPILER=clang++ -DLEXIGLANCE_FUZZ=ON -DLEXIGLANCE_SANITIZE=ON \
# -DLEXIGLANCE_BUILD_GUI=OFF -DLEXIGLANCE_BUILD_DAEMON=OFF
# build/fuzz/tests/fuzz/fuzzZip -max_total_time=60 corpus/zip
foreach(
    name
    Json
    Zip
    Config
    LineBuffer
    StyleSheet
    Glossary
    Dictionary
    Import)
    add_executable(fuzz${name} Fuzz${name}.cpp)
    target_link_libraries(fuzz${name} PRIVATE lexiglance::core
                                              lexiglance::options)
    target_link_options(fuzz${name} PRIVATE -fsanitize=fuzzer)
endforeach()
