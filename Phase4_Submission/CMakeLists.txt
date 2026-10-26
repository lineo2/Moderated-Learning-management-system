cmake_minimum_required(VERSION 3.16)
project(MLMS VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SQLite3 REQUIRED)
# libsodium: vcpkg (Windows) provides a CMake package; Linux/macOS use pkg-config.
find_package(unofficial-sodium CONFIG QUIET)
if(unofficial-sodium_FOUND)
  set(MLMS_SODIUM unofficial-sodium::sodium)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  set(MLMS_SODIUM PkgConfig::SODIUM)
endif()

# One library per layer (Presentation, Application, Data access, Domain+Infrastructure).
add_library(mlms_core STATIC
  src/Domain.cpp src/Infrastructure.cpp src/Database.cpp
  src/Repositories.cpp src/Services.cpp src/Console.cpp)
target_include_directories(mlms_core PUBLIC src)
target_link_libraries(mlms_core PUBLIC SQLite::SQLite3 ${MLMS_SODIUM})
if(MSVC)
  target_compile_options(mlms_core PUBLIC /W4)
else()
  target_compile_options(mlms_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(mlms src/main.cpp)
target_link_libraries(mlms PRIVATE mlms_core)

enable_testing()
add_executable(mlms_tests tests/tests.cpp)
target_link_libraries(mlms_tests PRIVATE mlms_core)
add_test(NAME mlms_tests COMMAND mlms_tests)
