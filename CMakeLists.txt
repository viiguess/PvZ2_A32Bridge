cmake_minimum_required(VERSION 3.22)
project(a32bridge LANGUAGES C CXX)

# a32bridge: run a 32-bit ARM Android game library inside a 64-bit process.
#
# The build produces lib<LIBNAME>.so (arm64). It stands in for the game's original ARMv7 library, which must ship
# next to it renamed to lib<LIBNAME>_a32.so. Build with the Android NDK toolchain for arm64-v8a, e.g.
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DLIBNAME=Game

set(LIBNAME Game CACHE STRING "Name of the game library to stand in for (produces lib<LIBNAME>.so)")
set(ORIG_SIG_HASH "" CACHE STRING
    "Signature.hashCode() of the original APK signer. Answered when the game checks its own signature, which \
changes when the APK is re-signed.")
option(A32_LOG "Log translator messages and the game's own log/stdio output to logcat" OFF)
option(A32_TRACE "Also log every guest->host stub and JNI call (very verbose)" OFF)
set(A32_DYNARMIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic" CACHE PATH "Dynarmic source tree (git submodule)")
set(A32_BOOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH
    "Boost sources (Dynarmic dependency): the git submodule, or a directory containing boost/")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dynarmic: the A32 frontend only, no tests. Android needs one local change (see patches/dynarmic-android.patch);
# it is applied here, once, to the checkout.
if(NOT EXISTS "${A32_DYNARMIC_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Dynarmic not found at ${A32_DYNARMIC_DIR}. Run: git submodule update --init")
endif()
set(A32_ANDROID_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-android.patch")
execute_process(COMMAND git apply --check --reverse "${A32_ANDROID_PATCH}"
                WORKING_DIRECTORY "${A32_DYNARMIC_DIR}" RESULT_VARIABLE A32_PATCH_APPLIED OUTPUT_QUIET ERROR_QUIET)
if(NOT A32_PATCH_APPLIED EQUAL 0)
  execute_process(COMMAND git apply "${A32_ANDROID_PATCH}"
                  WORKING_DIRECTORY "${A32_DYNARMIC_DIR}" RESULT_VARIABLE A32_PATCH_RESULT)
  if(NOT A32_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "could not apply ${A32_ANDROID_PATCH} to ${A32_DYNARMIC_DIR}")
  endif()
endif()

# Boost: only headers are needed (Dynarmic uses a few of its libraries). Three ways to provide them, in this order:
#   1. the git submodule third_party/boost with the needed libraries initialized (tools/init_boost.sh),
#   2. a directory with the flattened release layout (boost/version.hpp), via -DA32_BOOST_DIR=...,
#   3. otherwise the release archive is downloaded.
if(EXISTS "${A32_BOOST_DIR}/libs/config/include/boost/version.hpp")
  file(GLOB A32_BOOST_INCLUDES "${A32_BOOST_DIR}/libs/*/include" "${A32_BOOST_DIR}/libs/numeric/*/include")
  include_directories(${A32_BOOST_INCLUDES})
  set(A32_BOOST_ROOT "${A32_BOOST_DIR}/libs/config/include")
elseif(EXISTS "${A32_BOOST_DIR}/boost/version.hpp")
  set(A32_BOOST_ROOT "${A32_BOOST_DIR}")
else()
  include(FetchContent)
  FetchContent_Declare(boost_headers URL https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.bz2
                       DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Populate(boost_headers)
  set(A32_BOOST_ROOT "${boost_headers_SOURCE_DIR}")
endif()

set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(BOOST_ROOT ${A32_BOOST_ROOT} CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR ${A32_BOOST_ROOT} CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
add_subdirectory(${A32_DYNARMIC_DIR} dynarmic EXCLUDE_FROM_ALL)

file(GLOB_RECURSE A32_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${LIBNAME} SHARED ${A32_SOURCES})
target_include_directories(${LIBNAME} PRIVATE src)
target_link_libraries(${LIBNAME} PRIVATE dynarmic log z dl OpenSLES android)
target_compile_options(${LIBNAME} PRIVATE -O2 -fvisibility=hidden -Wall -Wno-writable-strings)
target_link_options(${LIBNAME} PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384)

if(A32_LOG OR A32_TRACE)
  target_compile_definitions(${LIBNAME} PRIVATE A32_LOG)
endif()
if(A32_TRACE)
  target_compile_definitions(${LIBNAME} PRIVATE A32_TRACE)
endif()
if(ORIG_SIG_HASH)
  target_compile_definitions(${LIBNAME} PRIVATE ORIG_SIG_HASH=${ORIG_SIG_HASH})
endif()
