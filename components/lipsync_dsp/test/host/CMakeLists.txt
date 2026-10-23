# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host-side (ESP-IDF independent) test for the formant vowel detector.
#
#   cmake -S . -B build && cmake --build build
#   ./build/vowel_test <dataset-dir>              # accuracy on a labelled set
#   ./build/vowel_test <dataset-dir> --compare <id> <proto-dump.tsv>
#
# The dataset is produced by tools/vowel-lipsync/gen_dataset.py (VOICEVOX),
# the reference dump by tools/vowel-lipsync/proto.py --dump-feat.

cmake_minimum_required(VERSION 3.16)
project(lipsync_dsp_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(DSP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(lipsync_dsp STATIC ${DSP_ROOT}/vowel_detector.cpp)
target_include_directories(lipsync_dsp PUBLIC ${DSP_ROOT}/include PRIVATE ${DSP_ROOT})
target_compile_options(lipsync_dsp PRIVATE -Wall -Wextra)

enable_testing()

add_executable(vowel_test test_vowel.cpp)
target_link_libraries(vowel_test PRIVATE lipsync_dsp)
target_compile_options(vowel_test PRIVATE -Wall -Wextra)

# Self-contained check (synthetic vowels, no dataset needed) for CI.
add_executable(vowel_selftest test_selftest.cpp)
target_link_libraries(vowel_selftest PRIVATE lipsync_dsp)
target_compile_options(vowel_selftest PRIVATE -Wall -Wextra)
add_test(NAME selftest COMMAND vowel_selftest)
