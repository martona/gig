cmake_minimum_required(VERSION 3.24)

cmake_policy(SET CMP0091 NEW)

project(frigate_d3d_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

add_executable(frigate_d3d_poc
    src/main.cpp
    src/video_frame.h
    src/decode/ffmpeg_decoder.cpp
    src/decode/ffmpeg_decoder.h
    src/render/d3d11_renderer.cpp
    src/render/video_renderer.h
)

target_include_directories(frigate_d3d_poc PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(frigate_d3d_poc PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(frigate_d3d_poc PRIVATE
    ${FFMPEG_LIBRARIES}
)

if(TARGET SDL2::SDL2main)
    target_link_libraries(frigate_d3d_poc PRIVATE SDL2::SDL2main)
endif()

if(TARGET SDL2::SDL2-static)
    target_link_libraries(frigate_d3d_poc PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(frigate_d3d_poc PRIVATE SDL2::SDL2)
else()
    message(FATAL_ERROR "SDL2 CMake target was not found.")
endif()

if(WIN32)
    target_compile_definitions(frigate_d3d_poc PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    target_link_libraries(frigate_d3d_poc PRIVATE
        d3d11
        dxgi
        d3dcompiler
    )
endif()

if(MSVC)
    target_compile_options(frigate_d3d_poc PRIVATE /W4 /permissive-)
else()
    target_compile_options(frigate_d3d_poc PRIVATE -Wall -Wextra -Wpedantic)
endif()
