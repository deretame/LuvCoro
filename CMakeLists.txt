cmake_minimum_required(VERSION 3.22)
project(LuvCoro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuv/CMakeLists.txt")
  message(FATAL_ERROR "libuv source is missing. Please populate third_party/libuv first (for example: python script/bootstrap.py deps).")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libcoro/CMakeLists.txt")
  message(FATAL_ERROR "libcoro source is missing. Please populate third_party/libcoro first (for example: python script/bootstrap.py deps).")
endif()

set(LIBCORO_FEATURE_NETWORKING OFF CACHE BOOL "" FORCE)
set(LIBCORO_FEATURE_TLS OFF CACHE BOOL "" FORCE)
set(LIBCORO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBCORO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)
add_subdirectory(third_party/libcoro EXCLUDE_FROM_ALL)

set(LIBUV_TARGET "")
if(TARGET uv_a)
  set(LIBUV_TARGET uv_a)
elseif(TARGET uv)
  set(LIBUV_TARGET uv)
else()
  message(FATAL_ERROR "Could not find libuv CMake target (uv_a/uv)")
endif()

set(LIBCORO_TARGET "")
if(TARGET libcoro)
  set(LIBCORO_TARGET libcoro)
elseif(TARGET coro)
  set(LIBCORO_TARGET coro)
elseif(TARGET coro::coro)
  set(LIBCORO_TARGET coro::coro)
else()
  message(FATAL_ERROR "Could not find libcoro CMake target")
endif()

add_library(luvcoro STATIC
  src/runtime.cpp
  src/dns.cpp
  src/fs.cpp
  src/fs_watch.cpp
  src/pipe.cpp
  src/process.cpp
  src/signal.cpp
  src/stdio.cpp
  src/tcp.cpp
  src/tty.cpp
  src/udp.cpp
)

add_library(luvcoro::luvcoro ALIAS luvcoro)

target_include_directories(luvcoro
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(luvcoro
  PUBLIC
    ${LIBCORO_TARGET}
    ${LIBUV_TARGET}
)

if(WIN32)
  target_compile_definitions(luvcoro PUBLIC _WIN32_WINNT=0x0A00)
endif()
