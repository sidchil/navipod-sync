cmake_minimum_required(VERSION 3.16)
project(navipod-sync VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

option(NAVIPOD_SYSTEM_LIBGPOD "Use an installed libgpod instead of extern/libgpod-ng" OFF)
option(NAVIPOD_BUILD_TESTS    "Build the tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the installed copy of the program's own libraries at run time
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
else()
  set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.32)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  pkg_check_modules(NLOHMANN REQUIRED IMPORTED_TARGET nlohmann_json)
  add_library(nlohmann_json::nlohmann_json ALIAS PkgConfig::NLOHMANN)
endif()

if(NAVIPOD_SYSTEM_LIBGPOD)
  find_package(libgpod REQUIRED)
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/libgpod-ng/CMakeLists.txt")
    message(FATAL_ERROR "extern/libgpod-ng is empty. Run: git submodule update --init")
  endif()
  set(GPOD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
  add_subdirectory(extern/libgpod-ng)
endif()

add_executable(navipod src/navipod.cpp)
target_link_libraries(navipod PRIVATE libgpod::gpod PkgConfig::GLIB CURL::libcurl nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(navipod PRIVATE -Wall)
endif()

install(TARGETS navipod RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NAVIPOD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
