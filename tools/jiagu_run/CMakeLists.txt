cmake_minimum_required(VERSION 3.22)
project(jiagu_run CXX C)

# jiagu_run: runs a 32-bit ARM Android library on a Linux host (WSL works) inside the a32bridge runtime, with a fake JNI
# environment. See README.md. Use tools/jiagu_run/build.sh rather than calling cmake directly.
#
# Options:
#   -DJR_TRACE=ON   log every guest -> host call (very verbose); JR_TRACE_LIMIT sets how many are logged

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(JR_TRACE "Log every guest->host call" OFF)
set(JR_TRACE_LIMIT 100000 CACHE STRING "Number of calls logged by JR_TRACE")

# Dynarmic and Boost: the same third-party sources the Android build uses (git submodules).
set(A32_DYNARMIC_DIR ${ROOT}/third_party/dynarmic)
set(A32_BOOST_DIR ${ROOT}/third_party/boost)
file(GLOB A32_BOOST_INCLUDES "${A32_BOOST_DIR}/libs/*/include" "${A32_BOOST_DIR}/libs/numeric/*/include")
include_directories(${A32_BOOST_INCLUDES})
set(A32_BOOST_ROOT "${A32_BOOST_DIR}/libs/config/include")

set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(BOOST_ROOT ${A32_BOOST_ROOT} CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR ${A32_BOOST_ROOT} CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
add_subdirectory(${A32_DYNARMIC_DIR} dynarmic EXCLUDE_FROM_ALL)

# The translation runtime and libc layer are shared with the Android bridge; the JNI layer is replaced by fake_jni.cpp.
file(GLOB RUNTIME_SOURCES ${ROOT}/src/runtime/*.cpp ${ROOT}/src/libc/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/(live_patch|gles_imports)\\.cpp$")

add_executable(jiagu_run
  main.cpp fake_java.cpp fake_jni.cpp host_env.cpp
  ${RUNTIME_SOURCES})
target_include_directories(jiagu_run PRIVATE ${ROOT}/src shim)
target_compile_definitions(jiagu_run PRIVATE A32_LOG A32_HOST_RUNNER)
if(JR_TRACE)
  target_compile_definitions(jiagu_run PRIVATE A32_TRACE A32_TRACE_LIMIT=${JR_TRACE_LIMIT})
endif()
# The shared sources were written for bionic / clang; keep their warnings out of this build's output.
target_compile_options(jiagu_run PRIVATE -O2 -w -fpermissive "SHELL:-include string.h" "SHELL:-include stdlib.h")
target_link_libraries(jiagu_run PRIVATE dynarmic z dl pthread)
