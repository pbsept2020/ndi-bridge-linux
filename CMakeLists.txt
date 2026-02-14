cmake_minimum_required(VERSION 3.16)
project(ndi-bridge-linux VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ---
option(BUILD_HOST "Build host (sender) mode" ON)
option(BUILD_JOIN "Build join (receiver) mode" ON)

# --- Find Dependencies ---

# NDI SDK
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(NDI REQUIRED)

# FFmpeg (optional during development, required for full functionality)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(AVCODEC libavcodec)
    pkg_check_modules(AVUTIL libavutil)
    pkg_check_modules(SWSCALE libswscale)
    pkg_check_modules(AVFORMAT libavformat)
endif()

# Fallback: manual detection for macOS Homebrew
if(NOT AVCODEC_FOUND AND APPLE)
    # Try Homebrew paths
    set(HOMEBREW_PREFIX "/opt/homebrew")
    if(EXISTS "${HOMEBREW_PREFIX}/include/libavcodec/avcodec.h")
        message(STATUS "Found FFmpeg via Homebrew")
        set(AVCODEC_FOUND TRUE)
        set(AVUTIL_FOUND TRUE)
        set(SWSCALE_FOUND TRUE)
        set(AVCODEC_INCLUDE_DIRS "${HOMEBREW_PREFIX}/include")
        set(AVUTIL_INCLUDE_DIRS "${HOMEBREW_PREFIX}/include")
        set(SWSCALE_INCLUDE_DIRS "${HOMEBREW_PREFIX}/include")
        set(AVCODEC_LIBRARY_DIRS "${HOMEBREW_PREFIX}/lib")
        set(AVUTIL_LIBRARY_DIRS "${HOMEBREW_PREFIX}/lib")
        set(SWSCALE_LIBRARY_DIRS "${HOMEBREW_PREFIX}/lib")
        set(AVCODEC_LIBRARIES "avcodec")
        set(AVUTIL_LIBRARIES "avutil")
        set(SWSCALE_LIBRARIES "swscale")
    endif()
endif()

if(NOT AVCODEC_FOUND)
    message(WARNING "FFmpeg not found - video encoding/decoding will not be available")
    message(WARNING "Install with: sudo apt install libavcodec-dev libavutil-dev libswscale-dev")
endif()

# libltc (LTC timecode encoding/decoding)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LTC ltc)
endif()

# Fallback manual detection for libltc
if(NOT LTC_FOUND)
    find_library(LTC_LIBRARIES NAMES ltc)
    find_path(LTC_INCLUDE_DIRS ltc.h)
    if(LTC_LIBRARIES AND LTC_INCLUDE_DIRS)
        set(LTC_FOUND TRUE)
    endif()
endif()

# Fallback: macOS Homebrew for libltc
if(NOT LTC_FOUND AND APPLE)
    set(HOMEBREW_PREFIX "/opt/homebrew")
    if(EXISTS "${HOMEBREW_PREFIX}/include/ltc.h")
        message(STATUS "Found libltc via Homebrew")
        set(LTC_FOUND TRUE)
        set(LTC_INCLUDE_DIRS "${HOMEBREW_PREFIX}/include")
        set(LTC_LIBRARY_DIRS "${HOMEBREW_PREFIX}/lib")
        set(LTC_LIBRARIES "ltc")
    endif()
endif()

if(LTC_FOUND)
    message(STATUS "Found libltc: ${LTC_LIBRARIES}")
else()
    message(WARNING "libltc not found - LTC timecode will not be available in test pattern")
    message(WARNING "Install with: brew install libltc (Mac) or sudo apt install libltc-dev (Linux)")
endif()

# Threads
find_package(Threads REQUIRED)

# --- Common Library ---
set(COMMON_SOURCES
    src/common/Protocol.cpp
    src/common/Logger.cpp
    src/network/NetworkSender.cpp
    src/network/NetworkReceiver.cpp
    src/ndi/NDIReceiver.cpp
    src/ndi/NDISender.cpp
    src/host/HostMode.cpp
    src/join/JoinMode.cpp
)

# Add FFmpeg-dependent sources
if(AVCODEC_FOUND)
    list(APPEND COMMON_SOURCES
        src/video/VideoEncoder.cpp
        src/video/VideoDecoder.cpp
    )
endif()

add_library(ndi_bridge_common STATIC ${COMMON_SOURCES})

target_include_directories(ndi_bridge_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${NDI_INCLUDE_DIRS}
)

target_link_libraries(ndi_bridge_common PUBLIC
    ${NDI_LIBRARIES}
    Threads::Threads
)

if(NDI_LIBRARY_DIRS)
    target_link_directories(ndi_bridge_common PUBLIC ${NDI_LIBRARY_DIRS})
endif()

if(AVCODEC_FOUND)
    target_include_directories(ndi_bridge_common PUBLIC
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
    )
    target_link_libraries(ndi_bridge_common PUBLIC
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
    )
    target_link_directories(ndi_bridge_common PUBLIC
        ${AVCODEC_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS}
    )
    target_compile_definitions(ndi_bridge_common PUBLIC HAVE_FFMPEG=1)
endif()

target_compile_options(ndi_bridge_common PRIVATE -Wall -Wextra -Wpedantic)

# --- macOS RPATH for NDI SDK ---
if(APPLE AND NDI_LIBRARY_DIR)
    set(CMAKE_INSTALL_RPATH "${NDI_LIBRARY_DIR}")
    set(CMAKE_BUILD_RPATH "${NDI_LIBRARY_DIR}")
endif()

# --- Main Executable ---
add_executable(ndi-bridge
    src/main.cpp
)

target_link_libraries(ndi-bridge PRIVATE
    ndi_bridge_common
    ${NDI_LIBRARIES}
    Threads::Threads
)

if(APPLE)
    target_link_libraries(ndi-bridge PRIVATE "-framework CoreFoundation")
endif()

target_include_directories(ndi-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${NDI_INCLUDE_DIRS}
)

if(NDI_LIBRARY_DIRS)
    target_link_directories(ndi-bridge PRIVATE ${NDI_LIBRARY_DIRS})
endif()

# --- Test Executables ---
option(BUILD_TESTS "Build test executables" ON)
if(BUILD_TESTS)
    # Network loopback test
    add_executable(network-test
        src/tests/network_test.cpp
    )
    target_link_libraries(network-test PRIVATE
        ndi_bridge_common
        Threads::Threads
    )
    target_include_directories(network-test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    # Encoder test (requires FFmpeg)
    add_executable(encoder-test
        src/tests/encoder_test.cpp
    )
    target_link_libraries(encoder-test PRIVATE
        ndi_bridge_common
        Threads::Threads
    )
    target_include_directories(encoder-test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    # Codec roundtrip test (requires FFmpeg)
    add_executable(codec-test
        src/tests/codec_test.cpp
    )
    target_link_libraries(codec-test PRIVATE
        ndi_bridge_common
        Threads::Threads
    )
    target_include_directories(codec-test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
endif()

# --- NDI Viewer (requires SDL2 + SDL2_ttf) ---
find_package(SDL2 QUIET)
if(SDL2_FOUND OR EXISTS "/usr/include/SDL2/SDL.h")
    # Check for SDL2_ttf
    if(EXISTS "/usr/include/SDL2/SDL_ttf.h")
        message(STATUS "SDL2 + SDL2_ttf found - building ndi-viewer with GUI")
        add_executable(ndi-viewer
            src/tools/ndi_viewer.cpp
        )
        target_include_directories(ndi-viewer PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${NDI_INCLUDE_DIRS}
            /usr/include/SDL2
        )
        target_link_libraries(ndi-viewer PRIVATE
            ${NDI_LIBRARIES}
            SDL2
            SDL2_ttf
            Threads::Threads
        )
        if(NDI_LIBRARY_DIRS)
            target_link_directories(ndi-viewer PRIVATE ${NDI_LIBRARY_DIRS})
        endif()
    else()
        message(STATUS "SDL2_ttf not found - skipping ndi-viewer")
        message(STATUS "Install with: sudo apt install libsdl2-ttf-dev")
    endif()
else()
    message(STATUS "SDL2 not found - skipping ndi-viewer")
    message(STATUS "Install with: sudo apt install libsdl2-dev libsdl2-ttf-dev")
endif()

# --- NDI Test Pattern Generator (with LTC timecode) ---
add_executable(ndi-test-pattern
    src/tools/ndi_test_pattern.cpp
)
target_include_directories(ndi-test-pattern PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${NDI_INCLUDE_DIRS}
)
target_link_libraries(ndi-test-pattern PRIVATE
    ${NDI_LIBRARIES}
    Threads::Threads
)
if(NDI_LIBRARY_DIRS)
    target_link_directories(ndi-test-pattern PRIVATE ${NDI_LIBRARY_DIRS})
endif()

if(LTC_FOUND)
    target_compile_definitions(ndi-test-pattern PRIVATE HAVE_LIBLTC=1)
    target_link_libraries(ndi-test-pattern PRIVATE ${LTC_LIBRARIES})
    if(LTC_INCLUDE_DIRS)
        target_include_directories(ndi-test-pattern PRIVATE ${LTC_INCLUDE_DIRS})
    endif()
    if(LTC_LIBRARY_DIRS)
        target_link_directories(ndi-test-pattern PRIVATE ${LTC_LIBRARY_DIRS})
    endif()
endif()

# --- Install ---
install(TARGETS ndi-bridge RUNTIME DESTINATION bin)

# --- Summary ---
message(STATUS "")
message(STATUS "=== NDI Bridge Linux Configuration ===")
message(STATUS "NDI SDK: ${NDI_INCLUDE_DIRS}")
message(STATUS "NDI Lib: ${NDI_LIBRARIES}")
if(AVCODEC_FOUND)
    message(STATUS "FFmpeg avcodec: ${AVCODEC_VERSION}")
    message(STATUS "FFmpeg avutil: ${AVUTIL_VERSION}")
    message(STATUS "FFmpeg swscale: ${SWSCALE_VERSION}")
else()
    message(STATUS "FFmpeg: NOT FOUND (encoding/decoding disabled)")
endif()
if(LTC_FOUND)
    message(STATUS "libltc: ${LTC_LIBRARIES}")
else()
    message(STATUS "libltc: NOT FOUND (LTC timecode disabled)")
endif()
message(STATUS "")
