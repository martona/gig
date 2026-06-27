cmake_minimum_required(VERSION 3.24)

cmake_policy(SET CMP0091 NEW)

project(gig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Version (W.X.Y.Z), tag-driven: scripts/build_windows.ps1 -Version passes
# -DGIG_VERSION (the release workflow derives it from the git tag). Plain local
# builds default to 0.0.0.0. The parts feed resources/version.rc.in.
if(NOT DEFINED GIG_VERSION OR GIG_VERSION STREQUAL "")
    set(GIG_VERSION "0.0.0.0")
endif()
if(NOT GIG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "GIG_VERSION must be W.X.Y.Z (four non-negative integers); got '${GIG_VERSION}'")
endif()
string(REPLACE "." ";" _gig_version_parts "${GIG_VERSION}")
list(GET _gig_version_parts 0 GIG_VERSION_MAJOR)
list(GET _gig_version_parts 1 GIG_VERSION_MINOR)
list(GET _gig_version_parts 2 GIG_VERSION_PATCH)
list(GET _gig_version_parts 3 GIG_VERSION_BUILD)
message(STATUS "gig version: ${GIG_VERSION}")

find_package(Boost REQUIRED COMPONENTS json)
find_package(SDL3 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(umbra CONFIG REQUIRED)

add_executable(gig WIN32
    src/main.cpp
    src/log.cpp
    src/log.hpp
    src/app/app_session.cpp
    src/app/app_session.h
    src/app/camera_supervisor.cpp
    src/app/camera_supervisor.h
    src/d3d11_decode_context.h
    src/video_frame.h
    src/decode/ffmpeg_decoder.cpp
    src/decode/ffmpeg_decoder.h
    src/discovery/frigate_discovery.cpp
    src/discovery/frigate_discovery.h
    src/health/stream_health.cpp
    src/health/stream_health.h
    src/net/cert_pin.cpp
    src/net/cert_pin.hpp
    src/net/cookie_jar.cpp
    src/net/cookie_jar.hpp
    src/net/frigate_auth.cpp
    src/net/frigate_auth.hpp
    src/net/http_client.cpp
    src/net/http_client.hpp
    src/net/tls_client.cpp
    src/net/tls_client.hpp
    src/net/tls_context.cpp
    src/net/tls_context.hpp
    src/net/tls_options.h
    src/net/tls_session_cache.cpp
    src/net/tls_session_cache.hpp
    src/net/url.h
    src/platform/settings_store.hpp
    src/platform/settings_store_win.cpp
    src/render/d3d11_renderer.cpp
    src/render/grid_layout.cpp
    src/render/grid_layout.h
    src/render/text_overlay.cpp
    src/render/text_overlay.h
    src/render/video_renderer.h
    src/ui/resource.h
    src/ui/settings_dialog.cpp
    src/ui/settings_dialog.h
)

target_include_directories(gig PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(gig PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(gig PRIVATE
    ${FFMPEG_LIBRARIES}
)

target_link_libraries(gig PRIVATE
    Boost::boost
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    SDL3::SDL3
    imgui::imgui
)

if(WIN32)
    enable_language(RC)

    # Stamp the version into a generated VERSIONINFO resource (kept separate from
    # gig.rc, which has no build-time tokens). Regenerated each configure, so a new
    # -DGIG_VERSION re-stamps the exe. No relative paths inside, so living under the
    # build tree is fine.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
        @ONLY)

    target_sources(gig PRIVATE
        resources/gig.rc
        "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
        # Windows-only TLS client-cert path (CNG bridge + cert-store picker).
        # The lazy client-cert callback drops off-Windows; login auth is the
        # cross-platform replacement. See tls_context.cpp (#ifdef _WIN32).
        src/net/cng_tls.cpp
        src/net/cng_tls.hpp
        src/net/win_cert_store.cpp
        src/net/win_cert_store.h
    )

    target_compile_definitions(gig PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    target_link_libraries(gig PRIVATE
        umbra::umbra
        d3d11
        dxgi
        d3dcompiler
        gdi32
        crypt32
        cryptui
        ncrypt
        uxtheme
        dwmapi
        comctl32
        ole32
    )

    # We embed our own application manifest via resources/gig.rc (Common Controls
    # v6 for umbra). Stop the linker generating a competing default manifest --
    # otherwise two RT_MANIFEST id 1 resources collide.
    target_link_options(gig PRIVATE /MANIFEST:NO)
endif()

if(MSVC)
    # /bigobj: the Boost.Beast TUs (http_client/tls_client) exceed the COFF section
    # limit once /Zi adds debug-info sections.
    target_compile_options(gig PRIVATE /W4 /permissive- /bigobj)
    # Ship symbols: emit a PDB for optimized builds too (separate file). /DEBUG
    # disables the size optimizations, so re-enable /OPT:REF + /OPT:ICF to keep the
    # exe lean. Release CI zips the PDB into a -symbols.zip for crash symbolication.
    target_compile_options(gig PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_link_options(gig PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)
else()
    target_compile_options(gig PRIVATE -Wall -Wextra -Wpedantic)
endif()
