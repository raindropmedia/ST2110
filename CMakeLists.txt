cmake_minimum_required(VERSION 3.24)
project(st2110 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ST2110_BUILD_TESTS "Unit-Tests bauen" ON)
option(ST2110_REQUIRE_MTL "Abbrechen, wenn MTL (pkg-config mtl) fehlt" OFF)
option(ST2110_DEV_WEBUI "Web-Oberfläche aus dem Quellordner laden" OFF)

include(cmake/Dependencies.cmake)

add_compile_options(-Wall -Wextra -Wpedantic)

# Kern ohne MTL: Optionen, Auswertung, JSON-Ausgabe (auch ohne Karte testbar).
add_library(st2110_core STATIC
    src/core/AncAnalyzer.cpp
    src/core/AudioFormat.cpp
    src/core/AudioPacketAnalyzer.cpp
    src/core/FrameRate.cpp
    src/core/Histogram.cpp
    src/core/HbrmtAnalyzer.cpp
    src/core/FlowMonitor.cpp
    src/core/FlowTable.cpp
    src/core/MonitorHub.cpp
    src/core/NetworkInfo.cpp
    src/core/PacketParser.cpp
    src/core/PixelFormat.cpp
    src/core/Preview.cpp
    src/core/Options.cpp
    src/core/PtpMonitor.cpp
    src/core/Report.cpp
    src/core/SapDirectory.cpp
    src/core/Sdp.cpp
    src/core/SyntheticSender.cpp
    src/core/SystemInfo.cpp
    src/core/System10.cpp
    src/core/TestSender.cpp
    src/core/TestSignal.cpp
    src/core/TimeSeries.cpp
    src/core/TimingStats.cpp
    src/core/VideoPacketAnalyzer.cpp
    src/core/VideoTimingModel.cpp
)
target_include_directories(st2110_core PUBLIC src)
target_include_directories(st2110_core SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(st2110_core PUBLIC nlohmann_json::nlohmann_json)

# Messprogramm: Demo und Web-Oberfläche immer, Empfang über MTL nur, wenn MTL gefunden wird
# (installiert über scripts/install-mtl.sh, mit dem Paket-Rückruf aus patches/mtl/).
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MTL IMPORTED_TARGET mtl)
endif()

add_executable(st2110-monitor
    src/app/main.cpp
    src/demo/DemoSource.cpp
    src/demo/DemoSystemProbe.cpp
    src/discovery/Discovery.cpp
    src/web/WebServer.cpp
)
target_link_libraries(st2110-monitor PRIVATE st2110_core httplib::httplib Threads::Threads)
if(ST2110_DEV_WEBUI)
    # Entwicklung: Oberfläche direkt aus dem Quellordner (Änderungen ohne Neubau sichtbar).
    set(ST2110_WEBUI_PATH "${CMAKE_SOURCE_DIR}/webui")
else()
    set(ST2110_WEBUI_PATH "${CMAKE_INSTALL_PREFIX}/share/st2110/webui")
endif()
target_compile_definitions(st2110-monitor PRIVATE
    ST2110_WEBUI_DIR="${ST2110_WEBUI_PATH}"
    ST2110_VERSION="${PROJECT_VERSION}")

if(MTL_FOUND)
    message(STATUS "MTL ${MTL_VERSION} gefunden: Empfang über MTL wird gebaut")
    target_sources(st2110-monitor PRIVATE src/mtl/MtlSource.cpp)
    target_link_libraries(st2110-monitor PRIVATE PkgConfig::MTL)
    target_compile_definitions(st2110-monitor PRIVATE ST2110_WITH_MTL)
elseif(ST2110_REQUIRE_MTL)
    message(FATAL_ERROR "MTL nicht gefunden (pkg-config mtl). PKG_CONFIG_PATH prüfen oder scripts/install-mtl.sh ausführen.")
else()
    message(WARNING "MTL nicht gefunden (pkg-config mtl): st2110-monitor kann nur --demo.")
endif()

install(TARGETS st2110-monitor RUNTIME DESTINATION bin)
install(DIRECTORY webui/ DESTINATION share/st2110/webui)

if(ST2110_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
