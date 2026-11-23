include(FetchContent)

# Abhängigkeiten in festen Versionen, keine Systempakete.
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.65.0
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
# Abhängigkeiten nur zum Bauen: "cmake --install" installiert nur st2110-monitor und die Oberfläche.
set(JSON_Install OFF CACHE INTERNAL "")
set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
# Web-Oberfläche nur über HTTP im Messnetz; keine Kompression, kein TLS.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")
# JPEG-Kodierer für die Bildvorschau (stb_image_write, gemeinfrei, nur Header).
# Fester Commit vom 2026-11-01; der spätere Fix vom 12.11. betrifft nur das Schreiben in Dateien,
# das hier abgeschaltet ist (STBI_WRITE_NO_STDIO).
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 61e70c5ce7bf0e0ac47e694f00c9da61bcdc7b32
)
FetchContent_MakeAvailable(nlohmann_json httplib stb)

if(ST2110_BUILD_TESTS)
    # doctest 2.4.11 verlangt cmake_minimum_required < 3.5, was CMake 4 ablehnt.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(doctest)
endif()
