cmake_minimum_required(VERSION 3.16)

# Use GCC-14 for better RISC-V support if available (must be before project())
find_program(GCC14 gcc-14)
find_program(GXX14 g++-14)

if(GCC14 AND GXX14)
    set(CMAKE_C_COMPILER ${GCC14} CACHE PATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER ${GXX14} CACHE PATH "CXX compiler" FORCE)
endif()

project(asr_cpp VERSION 3.0.0)

if(GCC14 AND GXX14)
    message(STATUS "Using GCC-14 for better RISC-V support")
else()
    message(STATUS "GCC-14 not found, using default compiler")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
endif()

# Ensure Release mode has proper optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# RISC-V specific flags for memory alignment
if(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv|RISCV")
    message(STATUS "Detected RISC-V architecture, adding alignment flags")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mstrict-align -fno-aggressive-loop-optimizations")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mstrict-align -fno-aggressive-loop-optimizations")
    add_definitions(-DFFTW_NO_SIMD)
endif()

# Find required packages
find_package(PkgConfig REQUIRED)

# Find PortAudio
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# Find libsndfile
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Find ONNX Runtime
find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS
    /usr/local/include/onnxruntime
    /usr/include/onnxruntime
    /opt/homebrew/include/onnxruntime
)

find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    PATHS
    /usr/local/lib
    /usr/lib
    /opt/homebrew/lib
)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR "ONNX Runtime not found. Please install ONNX Runtime C++ library.")
endif()

# Find cURL for downloading models
find_package(CURL REQUIRED)

# Find FFTW3
if(APPLE)
    find_library(FFTW3F_LIB_PATH NAMES fftw3f PATHS /opt/homebrew/lib /usr/local/lib)
    find_path(FFTW3_INCLUDE_DIRS NAMES fftw3.h PATHS /opt/homebrew/include /usr/local/include)
    if(NOT FFTW3F_LIB_PATH OR NOT FFTW3_INCLUDE_DIRS)
        message(FATAL_ERROR "FFTW3 not found. Please install with: brew install fftw")
    endif()
    set(FFTW3_LIBRARY_DIRS /opt/homebrew/lib)
else()
    pkg_check_modules(FFTW3 REQUIRED fftw3f)
endif()

# Options
option(USE_MCP "Enable MCP (Model Context Protocol) support" ON)
option(USE_AEC "Enable WebRTC AEC (Acoustic Echo Cancellation) for barge-in support" ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/inc)
include_directories(${ONNXRUNTIME_INCLUDE_DIR})
include_directories(${FFTW3_INCLUDE_DIRS})

# ============================================================================
# Build mode detection: SDK vs Standalone
#
# SDK mode:        dependencies are pre-built and installed to CMAKE_PREFIX_PATH
#                  (triggered by: lunch k3-com260-voice-agent && ./build/build.sh cmake)
# Standalone mode: dependencies are built from source via add_subdirectory
#                  (triggered by: cmake -B build && make -C build)
# ============================================================================
find_library(_PROBE_SPACE_AUDIO NAMES space_audio QUIET)
if(_PROBE_SPACE_AUDIO)
    set(VOICE_AGENT_SDK_MODE TRUE)
    message(STATUS "Build mode: SDK (using pre-built libraries from CMAKE_PREFIX_PATH)")
else()
    set(VOICE_AGENT_SDK_MODE FALSE)
    message(STATUS "Build mode: Standalone (building dependencies from source)")
endif()
unset(_PROBE_SPACE_AUDIO CACHE)

# ============================================================================
# Resolve component dependencies
# ============================================================================
if(VOICE_AGENT_SDK_MODE)
    # --- SDK mode: find pre-built libraries ---

    # audio
    find_library(SPACE_AUDIO_LIB NAMES space_audio REQUIRED)
    find_library(AUDIO_RESAMPLER_LIB NAMES audio_resampler REQUIRED)
    find_path(AUDIO_INCLUDE_DIR NAMES audio_duplex.hpp REQUIRED)

    # stt
    find_library(STT_LIB NAMES stt REQUIRED)
    find_library(SENSEVOICE_LIB NAMES sensevoice REQUIRED)
    find_path(STT_INCLUDE_DIR NAMES stt_api.hpp REQUIRED)

    # tts (headers installed to include/tts/)
    find_library(TTS_LIB NAMES tts REQUIRED)
    find_path(TTS_INCLUDE_DIR NAMES tts_api.hpp PATH_SUFFIXES tts REQUIRED)

    # vad
    find_library(VAD_LIB NAMES vad REQUIRED)
    find_library(SILERO_VAD_LIB NAMES silero_vad REQUIRED)
    find_path(VAD_INCLUDE_DIR NAMES vad_api.hpp REQUIRED)

    # llm
    find_library(LLM_LIB NAMES llm_service_cpp REQUIRED)
    find_path(LLM_INCLUDE_DIR NAMES llm_service.h REQUIRED)

    # mcp
    if(USE_MCP)
        find_library(MCP_LIB NAMES mcp REQUIRED)
        find_path(MCP_INCLUDE_DIR NAMES mcp_api.hpp REQUIRED)
        find_package(nlohmann_json QUIET)
        if(NOT nlohmann_json_FOUND)
            find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
        endif()
        message(STATUS "MCP support enabled")
    endif()

else()
    # --- Standalone mode: build from source via add_subdirectory ---

    # Disable submodule optional builds
    set(AUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(AUDIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
    set(BUILD_STREAM_DEMO OFF CACHE BOOL "" FORCE)
    set(BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    set(BUILD_SIMPLE_DEMO OFF CACHE BOOL "" FORCE)
    set(BUILD_VAD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_VAD_PYTHON OFF CACHE BOOL "" FORCE)
    set(BUILD_LLM_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB OFF CACHE BOOL "Force static LLM library" FORCE)

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../multimedia/audio ${CMAKE_BINARY_DIR}/_deps/audio)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../model_zoo/stt ${CMAKE_BINARY_DIR}/_deps/stt)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../model_zoo/tts ${CMAKE_BINARY_DIR}/_deps/tts)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../model_zoo/vad ${CMAKE_BINARY_DIR}/_deps/vad)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../model_zoo/llm ${CMAKE_BINARY_DIR}/_deps/llm)

    if(USE_MCP)
        set(MCP_BUILD_EXAMPLES OFF CACHE BOOL "Disable MCP examples" FORCE)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mcp ${CMAKE_BINARY_DIR}/_deps/mcp)
        message(STATUS "MCP support enabled")
    endif()
endif()

# ============================================================================
# WebRTC Audio Processing for AEC
# Always fetch + build to ~/.cache/thirdparty/webrtc-audio-processing (same as lidar).
# No find_library: webrtc is not required in staging.
# ============================================================================
if(USE_AEC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/webrtc_audio_processing.cmake)

    message(STATUS "WebRTC AEC support enabled")
    message(STATUS "WebRTC library: ${WEBRTC_LIB}")
    message(STATUS "WebRTC include dirs: ${WEBRTC_INCLUDE_DIRS}")
endif()

# Copy models directory if it exists
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/models DESTINATION ${CMAKE_BINARY_DIR})
endif()

# ============================================================================
# voice_chat_aec - Full-duplex voice chat with WebRTC AEC + barge-in
# ============================================================================
if(USE_AEC)
    set(VOICE_CHAT_AEC_SOURCES
        examples/main_voice_chat_aec.cpp
        src/aec_duplex_processor.cpp
        src/text_buffer.cpp
    )

    add_executable(voice_chat_aec ${VOICE_CHAT_AEC_SOURCES})

    target_compile_definitions(voice_chat_aec PRIVATE USE_AEC)

    if(NOT VOICE_AGENT_SDK_MODE)
        add_dependencies(voice_chat_aec webrtc_ap_ep)
    endif()

    if(VOICE_AGENT_SDK_MODE)
        # SDK mode: use found paths and libraries
        target_include_directories(voice_chat_aec PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${AUDIO_INCLUDE_DIR}
            ${STT_INCLUDE_DIR}
            ${TTS_INCLUDE_DIR}
            ${VAD_INCLUDE_DIR}
            ${LLM_INCLUDE_DIR}
            ${ONNXRUNTIME_INCLUDE_DIR}
            ${WEBRTC_INCLUDE_DIRS}
        )

        target_link_libraries(voice_chat_aec
            ${SPACE_AUDIO_LIB}
            ${AUDIO_RESAMPLER_LIB}
            ${STT_LIB}
            ${SENSEVOICE_LIB}
            ${TTS_LIB}
            ${VAD_LIB}
            ${SILERO_VAD_LIB}
            ${LLM_LIB}
            ${ONNXRUNTIME_LIB}
            ${CURL_LIBRARIES}
            ${WEBRTC_LIB}
            ${PORTAUDIO_LIBRARIES}
            ${SNDFILE_LIBRARIES}
            ${FFTW3_LIBRARIES}
            pthread
            m
        )
    else()
        # Standalone mode: use CMake target names (transitive deps resolved automatically)
        target_include_directories(voice_chat_aec PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${ONNXRUNTIME_INCLUDE_DIR}
            ${WEBRTC_INCLUDE_DIRS}
        )

        target_link_libraries(voice_chat_aec
            space_audio
            audio_resampler
            stt
            tts
            vad
            llm
            ${ONNXRUNTIME_LIB}
            ${CURL_LIBRARIES}
            ${WEBRTC_LIB}
            pthread
        )
    endif()

    target_link_directories(voice_chat_aec PRIVATE
        ${PORTAUDIO_LIBRARY_DIRS}
        ${SNDFILE_LIBRARY_DIRS}
        ${FFTW3_LIBRARY_DIRS}
    )

    set_target_properties(voice_chat_aec PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    # Set RPATH so the executable can find shared libs at runtime
    if(VOICE_AGENT_SDK_MODE)
        if(APPLE)
            set_target_properties(voice_chat_aec PROPERTIES
                INSTALL_RPATH "@executable_path/../lib"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        else()
            set_target_properties(voice_chat_aec PROPERTIES
                INSTALL_RPATH "$ORIGIN/../lib"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        endif()
    else()
        if(APPLE)
            set_target_properties(voice_chat_aec PROPERTIES
                INSTALL_RPATH "@executable_path/../lib;${WEBRTC_BUILD_DIR}/webrtc/modules/audio_processing"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        else()
            set_target_properties(voice_chat_aec PROPERTIES
                INSTALL_RPATH "$ORIGIN/../lib:${WEBRTC_BUILD_DIR}/webrtc/modules/audio_processing"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
        endif()
    endif()

    message(STATUS "Building voice_chat_aec with WebRTC AEC + full-duplex audio")

    # Optional MCP support
    if(USE_MCP)
        target_compile_definitions(voice_chat_aec PRIVATE USE_MCP)

        if(VOICE_AGENT_SDK_MODE)
            target_include_directories(voice_chat_aec PRIVATE ${MCP_INCLUDE_DIR})
            if(nlohmann_json_FOUND)
                target_link_libraries(voice_chat_aec nlohmann_json::nlohmann_json)
            elseif(NLOHMANN_JSON_INCLUDE_DIR)
                target_include_directories(voice_chat_aec PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
            endif()
            target_link_libraries(voice_chat_aec ${MCP_LIB})
        else()
            # Standalone: mcp built via add_subdirectory, nlohmann/json via FetchContent
            include(FetchContent)
            FetchContent_GetProperties(json)
            target_include_directories(voice_chat_aec BEFORE PRIVATE
                ${json_SOURCE_DIR}/include
            )
            target_link_libraries(voice_chat_aec mcp_static)
        endif()
        message(STATUS "  + MCP support enabled for voice_chat_aec")
    endif()

    # Install
    install(TARGETS voice_chat_aec RUNTIME DESTINATION bin)
endif()

# ============================================================================
# voice_chat - Voice chat without AEC (for hardware with built-in AEC)
# ============================================================================
set(VOICE_CHAT_SOURCES
    examples/main_voice_chat.cpp
    src/text_buffer.cpp
)

add_executable(voice_chat ${VOICE_CHAT_SOURCES})

if(VOICE_AGENT_SDK_MODE)
    target_include_directories(voice_chat PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${AUDIO_INCLUDE_DIR}
        ${STT_INCLUDE_DIR}
        ${TTS_INCLUDE_DIR}
        ${VAD_INCLUDE_DIR}
        ${LLM_INCLUDE_DIR}
        ${ONNXRUNTIME_INCLUDE_DIR}
    )

    target_link_libraries(voice_chat
        ${SPACE_AUDIO_LIB}
        ${AUDIO_RESAMPLER_LIB}
        ${STT_LIB}
        ${SENSEVOICE_LIB}
        ${TTS_LIB}
        ${VAD_LIB}
        ${SILERO_VAD_LIB}
        ${LLM_LIB}
        ${ONNXRUNTIME_LIB}
        ${CURL_LIBRARIES}
        ${PORTAUDIO_LIBRARIES}
        ${SNDFILE_LIBRARIES}
        ${FFTW3_LIBRARIES}
        pthread
        m
    )
else()
    target_include_directories(voice_chat PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${ONNXRUNTIME_INCLUDE_DIR}
    )

    target_link_libraries(voice_chat
        space_audio
        audio_resampler
        stt
        tts
        vad
        llm
        ${ONNXRUNTIME_LIB}
        ${CURL_LIBRARIES}
        pthread
    )
endif()

target_link_directories(voice_chat PRIVATE
    ${PORTAUDIO_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
    ${FFTW3_LIBRARY_DIRS}
)

set_target_properties(voice_chat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Set RPATH
if(VOICE_AGENT_SDK_MODE)
    if(APPLE)
        set_target_properties(voice_chat PROPERTIES
            INSTALL_RPATH "@executable_path/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    else()
        set_target_properties(voice_chat PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()
else()
    if(APPLE)
        set_target_properties(voice_chat PROPERTIES
            INSTALL_RPATH "@executable_path/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    else()
        set_target_properties(voice_chat PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()
endif()

message(STATUS "Building voice_chat (no AEC, independent capture/playback)")

# Optional MCP support
if(USE_MCP)
    target_compile_definitions(voice_chat PRIVATE USE_MCP)

    if(VOICE_AGENT_SDK_MODE)
        target_include_directories(voice_chat PRIVATE ${MCP_INCLUDE_DIR})
        if(nlohmann_json_FOUND)
            target_link_libraries(voice_chat nlohmann_json::nlohmann_json)
        elseif(NLOHMANN_JSON_INCLUDE_DIR)
            target_include_directories(voice_chat PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
        endif()
        target_link_libraries(voice_chat ${MCP_LIB})
    else()
        include(FetchContent)
        FetchContent_GetProperties(json)
        target_include_directories(voice_chat BEFORE PRIVATE
            ${json_SOURCE_DIR}/include
        )
        target_link_libraries(voice_chat mcp_static)
    endif()
    message(STATUS "  + MCP support enabled for voice_chat")
endif()

# Install
install(TARGETS voice_chat RUNTIME DESTINATION bin)
