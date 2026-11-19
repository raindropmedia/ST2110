cmake_minimum_required(VERSION 3.24)
project(st2110 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ST2110_BUILD_TESTS "Unit-Tests bauen" ON)
option(ST2110_REQUIRE_MTL "Abbrechen, wenn MTL (pkg-config mtl) fehlt" OFF)

include(cmake/Dependencies.cmake)

add_compile_options(-Wall -Wextra -Wpedantic)

# Kern ohne MTL: Optionen, Auswertung, JSON-Ausgabe (auch ohne Karte testbar).
add_library(st2110_core STATIC
    src/core/FrameRate.cpp
    src/core/Options.cpp
    src/core/Report.cpp
    src/core/TimingStats.cpp
)
target_include_directories(st2110_core PUBLIC src)
target_link_libraries(st2110_core PUBLIC nlohmann_json::nlohmann_json)

# Messprogramm mit MTL (Media Transport Library, installiert über scripts/install-mtl.sh).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MTL IMPORTED_TARGET mtl)
endif()

if(MTL_FOUND)
    message(STATUS "MTL ${MTL_VERSION} gefunden: st20-timing wird gebaut")
    find_package(Threads REQUIRED)
    add_executable(st20-timing
        src/app/main.cpp
        src/mtl/St20TimingReceiver.cpp
    )
    target_link_libraries(st20-timing PRIVATE st2110_core PkgConfig::MTL Threads::Threads)
    install(TARGETS st20-timing RUNTIME DESTINATION bin)
elseif(ST2110_REQUIRE_MTL)
    message(FATAL_ERROR "MTL nicht gefunden (pkg-config mtl). PKG_CONFIG_PATH prüfen oder scripts/install-mtl.sh ausführen.")
else()
    message(WARNING "MTL nicht gefunden (pkg-config mtl): nur Kern und Tests werden gebaut.")
endif()

if(ST2110_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
