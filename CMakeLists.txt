cmake_minimum_required(VERSION 3.24)

cmake_policy(SET CMP0091 NEW)

project(gig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Boost REQUIRED COMPONENTS json)
find_package(SDL3 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(gig
    src/main.cpp
    src/log.cpp
    src/log.hpp
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
    src/net/tls_options.h
    src/net/tls_session_cache.cpp
    src/net/tls_session_cache.hpp
    src/net/url.h
    src/net/http_client.cpp
    src/net/http_client.hpp
    src/net/win_cert_store.cpp
    src/net/win_cert_store.h
    src/probe/cert_probe.cpp
    src/probe/cert_probe.h
    src/probe/http_probe.cpp
    src/probe/http_probe.h
    src/render/d3d11_renderer.cpp
    src/render/grid_layout.cpp
    src/render/grid_layout.h
    src/render/text_overlay.cpp
    src/render/text_overlay.h
    src/render/video_renderer.h
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
)

if(WIN32)
    enable_language(RC)
    target_sources(gig PRIVATE resources/gig.rc)

    target_compile_definitions(gig PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    target_link_libraries(gig PRIVATE
        d3d11
        dxgi
        d3dcompiler
        gdi32
        crypt32
        cryptui
        ncrypt
    )
endif()

if(MSVC)
    target_compile_options(gig PRIVATE /W4 /permissive-)
else()
    target_compile_options(gig PRIVATE -Wall -Wextra -Wpedantic)
endif()
