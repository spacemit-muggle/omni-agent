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

# libsamplerate (transitive dependency of audio_resampler static lib)
pkg_check_modules(SAMPLERATE QUIET samplerate)

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
option(USE_AEC "Enable WebRTC AEC (Acoustic Echo Cancellation) for barge-in support" OFF)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${ONNXRUNTIME_INCLUDE_DIR})
include_directories(${FFTW3_INCLUDE_DIRS})

# ============================================================================
# Resolve component dependencies (SDK mode: pre-built libraries)
# ============================================================================

# audio
find_library(SPACE_AUDIO_LIB NAMES space_audio REQUIRED)
find_library(AUDIO_RESAMPLER_LIB NAMES audio_resampler REQUIRED)
find_path(AUDIO_INCLUDE_DIR NAMES audio_duplex.hpp REQUIRED)

# stt
find_library(STT_LIB NAMES stt REQUIRED)
find_library(SENSEVOICE_LIB NAMES sensevoice REQUIRED)
find_path(STT_INCLUDE_DIR NAMES asr_service.h REQUIRED)

# tts (headers installed to include/tts/)
find_library(TTS_LIB NAMES tts REQUIRED)
find_path(TTS_INCLUDE_DIR NAMES tts_service.h PATH_SUFFIXES tts REQUIRED)

# vad
find_library(VAD_LIB NAMES vad REQUIRED)
find_library(SILERO_VAD_LIB NAMES silero_vad REQUIRED)
find_path(VAD_INCLUDE_DIR NAMES vad_service.h REQUIRED)

# llm
find_library(LLM_LIB NAMES llm_service_cpp REQUIRED)
find_path(LLM_INCLUDE_DIR NAMES llm_service.h REQUIRED)

# mcp
if(USE_MCP)
    find_library(MCP_LIB NAMES mcp REQUIRED)
    find_path(MCP_INCLUDE_DIR NAMES mcp_service.hpp REQUIRED)
    find_package(nlohmann_json QUIET)
    if(NOT nlohmann_json_FOUND)
        find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
    endif()
    message(STATUS "MCP support enabled")
endif()

# ============================================================================
# WebRTC Audio Processing for AEC
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
# Common variables for both targets
# ============================================================================
set(VOICE_AGENT_COMMON_SOURCES
    src/voice_common.cpp
    src/mcp_helper.cpp
    src/engine_init.cpp
    src/voice_pipeline.cpp
    src/text_buffer.cpp
)

set(VOICE_AGENT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AUDIO_INCLUDE_DIR}
    ${STT_INCLUDE_DIR}
    ${TTS_INCLUDE_DIR}
    ${VAD_INCLUDE_DIR}
    ${LLM_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

set(VOICE_AGENT_LIBS
    ${SPACE_AUDIO_LIB}
    ${AUDIO_RESAMPLER_LIB}
    ${SAMPLERATE_LIBRARIES}
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

set(VOICE_AGENT_LINK_DIRS
    ${PORTAUDIO_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
    ${FFTW3_LIBRARY_DIRS}
    ${SAMPLERATE_LIBRARY_DIRS}
)

# ============================================================================
# voice_chat_aec - Full-duplex voice chat with WebRTC AEC + barge-in
# ============================================================================
if(USE_AEC)
    add_executable(voice_chat_aec
        examples/voice_chat_aec.cpp
        src/aec_duplex_processor.cpp
        ${VOICE_AGENT_COMMON_SOURCES}
    )

    target_compile_definitions(voice_chat_aec PRIVATE USE_AEC)
    add_dependencies(voice_chat_aec webrtc_ap_ep)

    target_include_directories(voice_chat_aec PRIVATE
        ${VOICE_AGENT_INCLUDE_DIRS}
        ${WEBRTC_INCLUDE_DIRS}
    )

    target_link_libraries(voice_chat_aec
        ${VOICE_AGENT_LIBS}
        ${WEBRTC_LIB}
    )

    target_link_directories(voice_chat_aec PRIVATE ${VOICE_AGENT_LINK_DIRS})

    set_target_properties(voice_chat_aec PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

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

    message(STATUS "Building voice_chat_aec with WebRTC AEC + full-duplex audio")

    if(USE_MCP)
        target_compile_definitions(voice_chat_aec PRIVATE USE_MCP)
        target_include_directories(voice_chat_aec PRIVATE ${MCP_INCLUDE_DIR})
        if(nlohmann_json_FOUND)
            target_link_libraries(voice_chat_aec nlohmann_json::nlohmann_json)
        elseif(NLOHMANN_JSON_INCLUDE_DIR)
            target_include_directories(voice_chat_aec PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
        endif()
        target_link_libraries(voice_chat_aec ${MCP_LIB})
        message(STATUS "  + MCP support enabled for voice_chat_aec")
    endif()

    # Install WebRTC shared library to lib/
    get_filename_component(_WEBRTC_LIB_DIR "${WEBRTC_LIB}" DIRECTORY)
    install(FILES "${_WEBRTC_LIB_DIR}/libwebrtc-audio-processing-2.so.1" DESTINATION lib)
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
        libwebrtc-audio-processing-2.so.1
        \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/libwebrtc-audio-processing-2.so)")

    install(TARGETS voice_chat_aec RUNTIME DESTINATION bin)
endif()

# ============================================================================
# voice_chat - Voice chat without AEC (for hardware with built-in AEC)
# ============================================================================
add_executable(voice_chat
    examples/voice_chat.cpp
    ${VOICE_AGENT_COMMON_SOURCES}
)

target_include_directories(voice_chat PRIVATE ${VOICE_AGENT_INCLUDE_DIRS})

target_link_libraries(voice_chat ${VOICE_AGENT_LIBS})

target_link_directories(voice_chat PRIVATE ${VOICE_AGENT_LINK_DIRS})

set_target_properties(voice_chat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

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

message(STATUS "Building voice_chat (no AEC, independent capture/playback)")

if(USE_MCP)
    target_compile_definitions(voice_chat PRIVATE USE_MCP)
    target_include_directories(voice_chat PRIVATE ${MCP_INCLUDE_DIR})
    if(nlohmann_json_FOUND)
        target_link_libraries(voice_chat nlohmann_json::nlohmann_json)
    elseif(NLOHMANN_JSON_INCLUDE_DIR)
        target_include_directories(voice_chat PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    endif()
    target_link_libraries(voice_chat ${MCP_LIB})
    message(STATUS "  + MCP support enabled for voice_chat")
endif()

install(TARGETS voice_chat RUNTIME DESTINATION bin)

