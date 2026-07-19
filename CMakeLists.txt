cmake_minimum_required(VERSION 3.20)
project(1code CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(
  -Wall -Wextra
  -Wno-unused-parameter
  -Wno-missing-field-initializers
)

# ---------------------------------------------------------------------------
# editor_core -- all editing logic. Deliberately links nothing: no SDL, no
# graphics. A stray SDL include in core/ fails the build, which is what keeps
# the core headlessly testable.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp)

add_library(editor_core STATIC ${CORE_SOURCES})
target_include_directories(editor_core PUBLIC core)

# ---------------------------------------------------------------------------
# editor -- the application. SDL lives only above this line.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)
  find_package(SDL3 CONFIG REQUIRED)
  file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS app/*.cpp)

  add_executable(editor ${APP_SOURCES})
  target_include_directories(editor PRIVATE app third_party)
  target_link_libraries(editor PRIVATE editor_core SDL3::SDL3)
endif()

# ---------------------------------------------------------------------------
# editor_tests -- links editor_core only, needs no display.
# ---------------------------------------------------------------------------
enable_testing()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)

add_executable(editor_tests ${TEST_SOURCES})
target_include_directories(editor_tests PRIVATE tests)
target_link_libraries(editor_tests PRIVATE editor_core)

add_test(NAME editor_tests COMMAND editor_tests)
