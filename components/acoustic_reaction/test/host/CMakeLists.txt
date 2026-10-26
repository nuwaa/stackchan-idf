cmake_minimum_required(VERSION 3.16)
project(acoustic_reaction_host_test CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(acoustic_reaction_test
    test_reaction_engine.cpp
    ../../reaction_engine.cpp)
target_include_directories(acoustic_reaction_test PRIVATE ../../include)
target_compile_options(acoustic_reaction_test PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME acoustic_reaction COMMAND acoustic_reaction_test)

add_executable(ggwave_roundtrip_test
    test_ggwave_roundtrip.cpp
    ../../ggwave_receiver.cpp
    ../../../../third_party/ggwave/ggwave/ggwave.cpp)
target_include_directories(ggwave_roundtrip_test PRIVATE
    ../../include ../../../../third_party/ggwave)
target_compile_options(ggwave_roundtrip_test PRIVATE -Wall -Wextra)
add_test(NAME ggwave_roundtrip COMMAND ggwave_roundtrip_test)
