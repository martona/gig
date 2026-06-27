cmake_minimum_required(VERSION 3.24)

cmake_policy(SET CMP0091 NEW)

# Apple: derive the static vcpkg triplet, architecture, and deployment target
# BEFORE project() so the vcpkg toolchain (invoked at project()) builds deps for
# the right triplet. Mirrors clipp's proven setup. Overlay triplets live in
# vcpkg-triplets/ (also registered in vcpkg-configuration.json for manual/CI runs).
if(APPLE)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment version" FORCE)
    endif()
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-triplets"
        CACHE PATH "vcpkg overlay triplet directory")
    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        execute_process(COMMAND uname -m
            OUTPUT_VARIABLE GIG_HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(GIG_HOST_ARCH STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx-macos14" CACHE STRING "vcpkg target triplet" FORCE)
        elseif(GIG_HOST_ARCH STREQUAL "x86_64")
            set(VCPKG_TARGET_TRIPLET "x64-osx-macos14" CACHE STRING "vcpkg target triplet" FORCE)
        else()
            message(FATAL_ERROR "Unsupported macOS architecture: ${GIG_HOST_ARCH}")
        endif()
    endif()
    if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
        if(VCPKG_TARGET_TRIPLET MATCHES "^arm64-")
            set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture" FORCE)
        elseif(VCPKG_TARGET_TRIPLET MATCHES "^x64-")
            set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "macOS target architecture" FORCE)
        endif()
    endif()
endif()

project(gig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    # Code-signing knobs, driven by scripts/build_macos.sh from APPLE_CODESIGN_IDENTITY /
    # APPLE_TEAM_ID. A stable Developer ID signature is what makes the keychain
    # "Always Allow" persist across rebuilds. Consumed by the Xcode generator block
    # below; the Ninja path signs post-build in the script.
    option(GIG_MACOS_ENABLE_CODE_SIGNING "Codesign the .app (Xcode generator)" OFF)
    option(GIG_MACOS_ENABLE_HARDENED_RUNTIME "Enable hardened runtime (notarization prereq)" OFF)
    set(GIG_MACOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple Developer team ID for signing")
    set(GIG_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING "Apple code signing identity")
endif()

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
set(GIG_VERSION_3PART "${GIG_VERSION_MAJOR}.${GIG_VERSION_MINOR}.${GIG_VERSION_PATCH}")
message(STATUS "gig version: ${GIG_VERSION}")

find_package(Boost REQUIRED COMPONENTS json)
find_package(SDL3 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(imgui CONFIG REQUIRED)
# umbra (Win32 dark mode) is Windows-only -- found inside the if(WIN32) block.

# Platform-neutral sources. WIN32 (GUI subsystem) is honored only on Windows;
# MACOSX_BUNDLE (build a .app) only on Apple; each is ignored on the other. The
# Windows- and macOS-specific sources are added per platform further below.
add_executable(gig WIN32 MACOSX_BUNDLE
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
    src/render/grid_layout.cpp
    src/render/grid_layout.h
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
    imgui::imgui
)

if(WIN32)
    enable_language(RC)

    find_package(umbra CONFIG REQUIRED)

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
        # Windows renderer (D3D11 + GDI overlay), native settings dialog, and the
        # registry settings backend -- all Windows-only, isolated here.
        src/render/d3d11_renderer.cpp
        src/render/text_overlay.cpp
        src/render/text_overlay.h
        src/ui/resource.h
        src/ui/settings_dialog.cpp
        src/ui/settings_dialog.h
        src/platform/settings_store_win.cpp
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
elseif(APPLE)
    # macOS renderer (Metal stub for now) + Keychain/plist settings backend, both
    # Objective-C++ (.mm). Software-decode target; the real Metal renderer and
    # VideoToolbox come later. The window/event layer is SDL3 (cross-platform).
    target_sources(gig PRIVATE
        src/render/metal_renderer.mm
        src/platform/settings_store_mac.mm
        src/net/system_trust_mac.mm
        src/net/system_trust_mac.h
    )

    # App icon: generate gig.icns from the 1024^2 boat.png master at build time
    # (sips + iconutil, both preinstalled on macOS) -- no generated binary committed.
    set(GIG_ICNS "${CMAKE_CURRENT_BINARY_DIR}/generated/gig.icns")
    add_custom_command(
        OUTPUT "${GIG_ICNS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_icns.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/boat.png" "${GIG_ICNS}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/boat.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_icns.sh"
        COMMENT "Generating gig.icns from boat.png (sips + iconutil)"
        VERBATIM)
    set_source_files_properties("${GIG_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(gig PRIVATE "${GIG_ICNS}")

    # The .mm sources use ARC (no manual retain/release). Set the Xcode build
    # setting for the Xcode generator and the compile flag for Ninja/Make.
    set_target_properties(gig PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")
    target_compile_options(gig PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    # AppKit/Metal headers carry deprecations we don't act on yet.
    target_compile_options(gig PRIVATE "-Wno-deprecated-declarations")

    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)       # Keychain (settings_store_mac)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)   # CAMetalLayer
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(gig PRIVATE
        ${FOUNDATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
    )

    # CMake substitutes these target properties into resources/Info.plist.in at
    # build time (CFBundleShortVersionString = 3-part, CFBundleVersion = full).
    set_target_properties(gig PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "gig"
        MACOSX_BUNDLE_GUI_IDENTIFIER "stream.gig.app"
        MACOSX_BUNDLE_ICON_FILE "gig.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${GIG_VERSION_3PART}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${GIG_VERSION}"
        OUTPUT_NAME "gig"
    )

    # Code signing for the Xcode generator (the Ninja path signs post-build in
    # scripts/build_macos.sh). Pinning entitlements (get-task-allow=false) keeps the
    # Xcode-injected get-task-allow=true out, which notarization would reject.
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        if(GIG_MACOS_ENABLE_CODE_SIGNING)
            set_target_properties(gig PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${GIG_MACOS_CODE_SIGN_IDENTITY}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${GIG_MACOS_DEVELOPMENT_TEAM}"
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/gig.entitlements")
            if(GIG_MACOS_ENABLE_HARDENED_RUNTIME)
                set_target_properties(gig PROPERTIES
                    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "YES"
                    XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp")
            endif()
        else()
            set_target_properties(gig PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
        endif()
    endif()
endif()

if(MSVC)
    # /bigobj: the Boost.Beast TUs (http_client/tls_client) exceed the COFF section
    # limit once /Zi adds debug-info sections.
    target_compile_options(gig PRIVATE /W4 /permissive- /bigobj)
    # Ship symbols: emit a PDB for optimized builds too (separate file). /DEBUG
    # disables the size optimizations, so re-enable /OPT:REF + /OPT:ICF to keep the
    # exe lean. Release CI zips the PDB into a -symbols.zip for crash symbolication.
    #
    # /GL + /LTCG: whole-program optimization. The decode/render/TLS TUs are
    # separate, so this buys cross-TU inlining (a bit of runtime + some size) for
    # longer link time only -- non-Debug, alongside the symbol/opt flags above.
    # (/OPT:REF,ICF already make non-Debug links non-incremental, which /LTCG needs.)
    target_compile_options(gig PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/Zi>
        $<$<NOT:$<CONFIG:Debug>>:/GL>)
    target_link_options(gig PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
        $<$<NOT:$<CONFIG:Debug>>:/LTCG>)
else()
    target_compile_options(gig PRIVATE -Wall -Wextra -Wpedantic)
endif()
