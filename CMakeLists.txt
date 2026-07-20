cmake_minimum_required(VERSION 3.20)
project(1coder CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Binaries land in the build directory itself under every generator. Visual
# Studio is multi-config and would otherwise bury them in build/Debug/, which
# breaks every ./build/editor_tests path in the justfile and the workflows.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${config} config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR})
endforeach()

# Static CRT, so the shipped .exe needs no redistributable -- the same reasoning
# that pins SDL to a static build below.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Applied only to our targets so a vendored SDL build is not forced onto our
# warning set (and so SDL's own flags stay under its control).
if(MSVC)
  set(EDITOR_COMPILE_OPTIONS
    /W4
    /permissive-
    # Source is UTF-8 and so are the string literals in it. Without this MSVC
    # reads them in the active code page.
    /utf-8
    /wd4100  # unreferenced formal parameter
    /wd4996  # 'fopen': consider fopen_s
  )
else()
  set(EDITOR_COMPILE_OPTIONS
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
  )
endif()

# ---------------------------------------------------------------------------
# editor_core -- all editing logic. Deliberately links nothing: no SDL, no
# graphics. A stray SDL include in core/ fails the build, which is what keeps
# the core headlessly testable.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp)

add_library(editor_core STATIC ${CORE_SOURCES})
target_include_directories(editor_core PUBLIC core)
target_compile_options(editor_core PRIVATE ${EDITOR_COMPILE_OPTIONS})
target_link_libraries(editor_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# editor -- the application. SDL lives only above this line.
# Prefers vendored third_party/SDL; falls back to find_package. Optional so
# headless CI can build and run editor_tests without SDL.
# ---------------------------------------------------------------------------
set(EDITOR_SDL_TARGET "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt)
    set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL test library" FORCE)
    add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
    # With SDL_SHARED=OFF, SDL3::SDL3 aliases the static library.
    set(EDITOR_SDL_TARGET SDL3::SDL3)
    message(STATUS "Using vendored SDL3 from third_party/SDL")
  else()
    find_package(SDL3 CONFIG QUIET)
    if(SDL3_FOUND)
      set(EDITOR_SDL_TARGET SDL3::SDL3)
      message(STATUS "Using system SDL3")
    endif()
  endif()

  if(EDITOR_SDL_TARGET)
    file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS app/*.cpp)

    add_executable(editor ${APP_SOURCES})
    target_include_directories(editor PRIVATE app)
    # Vendored single-header libraries are not ours to keep warning-clean.
    target_include_directories(editor SYSTEM PRIVATE third_party)
    target_compile_options(editor PRIVATE ${EDITOR_COMPILE_OPTIONS})
    target_link_libraries(editor PRIVATE editor_core ${EDITOR_SDL_TARGET})
  else()
    message(STATUS "SDL3 not found; skipping editor (tests still build)")
  endif()
endif()

# ---------------------------------------------------------------------------
# editor_tests -- links editor_core only, needs no display.
# ---------------------------------------------------------------------------
enable_testing()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)

add_executable(fake_lsp_server tools/fake_lsp_server.cpp)
target_compile_options(fake_lsp_server PRIVATE ${EDITOR_COMPILE_OPTIONS})
target_link_libraries(fake_lsp_server PRIVATE editor_core)
if(WIN32)
  target_link_libraries(fake_lsp_server PRIVATE Shell32)
endif()

add_executable(editor_tests ${TEST_SOURCES})
target_include_directories(editor_tests PRIVATE tests app)
target_compile_options(editor_tests PRIVATE ${EDITOR_COMPILE_OPTIONS})
target_compile_definitions(editor_tests PRIVATE EDITOR_FAKE_LSP_PATH="$<TARGET_FILE:fake_lsp_server>")
target_link_libraries(editor_tests PRIVATE editor_core)
add_dependencies(editor_tests fake_lsp_server)

add_test(NAME editor_tests COMMAND editor_tests)
