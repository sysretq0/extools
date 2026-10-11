# SPDX-License-Identifier: GPL-3.0-only
# etools - cmd and dumpsys, in process, as a library plus two drop-in binaries.
cmake_minimum_required(VERSION 3.22)
project(etools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ETOOLS_INC "${CMAKE_SOURCE_DIR}/include")
set(DEVICE_LIB "${CMAKE_SOURCE_DIR}/third_party/lib")

foreach(required "${ETOOLS_INC}/etools/etools.h" "${DEVICE_LIB}/libbinder.so"
                 "${DEVICE_LIB}/libutils.so")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "missing ${required} - run ./scripts/setup.sh")
  endif()
endforeach()

# The library. Everything public is in include/etools; everything else is
# internal and may change without a version bump.
#
# The AOSP headers are the NEWEST supported release's, never the oldest: the
# binary links the DEVICE's libbinder at runtime, and on the Itanium C++ ABI a
# callee that ignores an extra register argument is harmless while a caller that
# never sets one is not. IServiceManager::listServices is flagless on API 26/27
# and takes an int from 28; built against the older headers the call passes no
# flags register, every newer libbinder reads it anyway, and the answer is an
# empty list with nothing erroring anywhere. The constraint that actually binds
# is symbol existence, which the compiler does not check - see the nm -u gate in
# scripts/check_symbols.sh.
set(AOSP_INC "${CMAKE_SOURCE_DIR}/third_party/aosp/include")

add_library(etools
  src/args.cpp
  src/binder.cpp
  src/device.cpp
  src/dump_types.cpp
  src/extflags.cpp
  src/section.cpp
)
target_include_directories(etools
  PUBLIC ${ETOOLS_INC}
  PRIVATE ${AOSP_INC} "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(etools
  PUBLIC  # consumers need the device's libraries at runtime too
    "${DEVICE_LIB}/libbinder.so"
    "${DEVICE_LIB}/libutils.so"
    log
)
# android::Vector's typeinfo is a hidden symbol in libutils, so RTTI must be off
# for this to link at all.
target_compile_options(etools PRIVATE -Wall -Wextra -fno-rtti)
find_package(Threads REQUIRED)
target_link_libraries(etools PUBLIC Threads::Threads)

# The two binaries. Both are front ends over the library above; the tool logic
# itself lives in src/tool.cpp and is compiled into each of them, because the
# tool's own translation unit must not be shared with the loop-free CLI path.
foreach(tool dumpsyse cmde)
  add_executable(${tool} src/tool.cpp src/${tool}_main.cpp)
  target_link_libraries(${tool} PRIVATE etools)
  target_include_directories(${tool} PRIVATE ${AOSP_INC} "${CMAKE_SOURCE_DIR}/src")
  target_compile_options(${tool} PRIVATE -Wall -Wextra -fno-rtti)
endforeach()

# Compatibility self-check. Compatibility has regressed before in this family of
# projects, so it gets a runnable check rather than a comment: this suite pins
# the parsers to AOSP's real behaviour for every release variant, and pins the
# --ext- namespace to never colliding with the tools' own flags.
add_executable(test_flags
  src/args.cpp
  src/device.cpp
  src/extflags.cpp
  src/section.cpp
  tests/test_flags.cpp
)
target_include_directories(test_flags PRIVATE ${ETOOLS_INC} "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_flags PRIVATE log)
target_compile_options(test_flags PRIVATE -Wall -Wextra)
