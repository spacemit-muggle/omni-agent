# webrtc_audio_processing: fetch + build via Meson (same pattern as lidar FetchThirdParty).
# No vendoring; fetch to ~/.cache/thirdparty/ at configure time.
# Requires: meson, ninja.
#
# Abseil: meson subproject (extracted to SOURCE_DIR/subprojects/).

if(DEFINED _WEBRTC_AP_LOADED)
  return()
endif()
set(_WEBRTC_AP_LOADED ON)

get_filename_component(_VA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${_VA_ROOT}/cmake/FetchThirdParty.cmake")

if(DEFINED ENV{SROBOTIS_THIRDPARTY_CACHE})
  set(_WEBRTC_CACHE "$ENV{SROBOTIS_THIRDPARTY_CACHE}")
elseif(DEFINED ENV{HOME})
  set(_WEBRTC_CACHE "$ENV{HOME}/.cache/thirdparty")
else()
  set(_WEBRTC_CACHE "${CMAKE_BINARY_DIR}/.cache/thirdparty")
endif()

set(_WEBRTC_GIT_REPO "https://github.com/spacemit-robotics/webrtc-audio-processing.git")
if(DEFINED ENV{SROBOTIS_WEBRTC_GIT_REPO})
  set(_WEBRTC_GIT_REPO "$ENV{SROBOTIS_WEBRTC_GIT_REPO}")
endif()

fetch_thirdparty(NAME webrtc-audio-processing
  GIT_REPO "${_WEBRTC_GIT_REPO}" GIT_REF "master"
  OUT_SOURCE_DIR _WEBRTC_SRC)

set(_WEBRTC_SOURCE_DIR "${_WEBRTC_SRC}")
set(_WEBRTC_BINARY_DIR "${_WEBRTC_CACHE}/webrtc-audio-processing/build")

find_program(MESON_EXE meson REQUIRED)
find_program(NINJA_EXE ninja REQUIRED)

include(ExternalProject)
ExternalProject_Add(webrtc_ap_ep
  SOURCE_DIR "${_WEBRTC_SOURCE_DIR}"
  BINARY_DIR "${_WEBRTC_BINARY_DIR}"
  CONFIGURE_COMMAND ${MESON_EXE} setup <BINARY_DIR> <SOURCE_DIR>
    --buildtype=release
  BUILD_COMMAND ${NINJA_EXE} -C <BINARY_DIR>
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${_WEBRTC_BINARY_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.so"
                  "${_WEBRTC_BINARY_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.dylib"
)

# Output paths (build happens at build time) - set in includer's scope (no PARENT_SCOPE for include())
set(WEBRTC_DIR "${_WEBRTC_SOURCE_DIR}")
set(WEBRTC_BUILD_DIR "${_WEBRTC_BINARY_DIR}")
if(APPLE)
  set(WEBRTC_LIB "${_WEBRTC_BINARY_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.dylib")
else()
  set(WEBRTC_LIB "${_WEBRTC_BINARY_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.so")
endif()

# Abseil: meson subproject (extracted to SOURCE_DIR/subprojects/ by meson wrap).
# Path exists after webrtc_ap_ep configure step.
set(_ABSEIL_SUBPROJECT_SRC "${_WEBRTC_SOURCE_DIR}/subprojects/abseil-cpp-20240722.0")
find_path(_ABSEIL_INCLUDE absl/base/config.h
  PATHS
    "${_ABSEIL_SUBPROJECT_SRC}"
    "${_WEBRTC_SOURCE_DIR}/subprojects/abseil-cpp"
    /usr/include /usr/local/include
  NO_DEFAULT_PATH
)
if(_ABSEIL_INCLUDE)
  set(_ABSEIL_DIR "${_ABSEIL_INCLUDE}")
else()
  # Not fetched yet; use expected path (valid after webrtc_ap_ep configure)
  set(_ABSEIL_DIR "${_ABSEIL_SUBPROJECT_SRC}")
endif()
set(ABSEIL_INCLUDE_DIR "${_ABSEIL_DIR}")

# WEBRTC_INCLUDE_DIRS must include abseil for consumer
set(WEBRTC_INCLUDE_DIRS
  "${_WEBRTC_SOURCE_DIR}"
  "${_WEBRTC_SOURCE_DIR}/webrtc"
  "${_WEBRTC_BINARY_DIR}"
  "${_WEBRTC_BINARY_DIR}/webrtc"
  "${_ABSEIL_DIR}"
)

# Parent must add_dependencies(voice_chat_aec webrtc_ap_ep) so webrtc builds first
