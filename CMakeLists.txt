cmake_minimum_required(VERSION 3.22)

# Fix for old CMake policies in dependencies (ogg/vorbis)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(NINJAM_VERSION_FALLBACK "0.0.0")
set(NINJAM_VERSION_TAG "")

# Build Intel and Apple Silicon slices by default. Keep these as normal cache
# settings so local builds and toolchains can override them explicitly.
option(UniversalBinary "Build universal binary for macOS" ON)

if (APPLE)
    if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version for the Intel slice")
    endif()
    if (UniversalBinary
        AND NOT DEFINED CMAKE_OSX_ARCHITECTURES
        AND "$ENV{CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures to build")
    endif()
endif()

foreach(_ninjam_version_env NINJAM_VERSION_TAG RELEASE_TAG GITHUB_REF_NAME)
    if (NOT NINJAM_VERSION_TAG AND DEFINED ENV{${_ninjam_version_env}} AND NOT "$ENV{${_ninjam_version_env}}" STREQUAL "")
        set(_ninjam_version_candidate "$ENV{${_ninjam_version_env}}")
        string(REGEX REPLACE "^refs/tags/" "" _ninjam_version_candidate "${_ninjam_version_candidate}")
        string(REGEX REPLACE "^[vV]" "" _ninjam_version_candidate_number "${_ninjam_version_candidate}")
        if (_ninjam_version_candidate_number MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
            set(NINJAM_VERSION_TAG "${_ninjam_version_candidate}")
        endif()
    endif()
endforeach()
if (NOT NINJAM_VERSION_TAG)
    execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE NINJAM_VERSION_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
string(REGEX REPLACE "^refs/tags/" "" NINJAM_VERSION_TAG "${NINJAM_VERSION_TAG}")
string(REGEX REPLACE "^[vV]" "" NINJAM_PLUGIN_VERSION "${NINJAM_VERSION_TAG}")
if (NOT NINJAM_PLUGIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    set(NINJAM_PLUGIN_VERSION "${NINJAM_VERSION_FALLBACK}")
endif()
set(NINJAM_DISPLAY_VERSION "v${NINJAM_PLUGIN_VERSION}")

project(NINJAM_VST3 VERSION ${NINJAM_PLUGIN_VERSION})

if (APPLE)
    message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    message(STATUS "macOS architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()

set(NINJAM_BRAND_NAME "NINJAMplus")
set(NINJAM_VENDOR_NAME "AMP")
set(NINJAM_OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/${NINJAM_BRAND_NAME}")

# Enable C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linux LV2 is a shared module and links the fetched Ogg/Vorbis/Opus static
# libraries, so those dependencies must be built with -fPIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Fetch JUCE
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
)
FetchContent_MakeAvailable(JUCE)

if (WIN32)
    FetchContent_Declare(
        asiosdk
        GIT_REPOSITORY https://github.com/audiosdk/asio.git
        GIT_TAG        main
    )
    FetchContent_MakeAvailable(asiosdk)
endif()

# Fetch Ogg
FetchContent_Declare(
    ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG        v1.3.5
)
FetchContent_MakeAvailable(ogg)

# Help Vorbis find Ogg
set(OGG_INCLUDE_DIR ${ogg_SOURCE_DIR}/include)
set(OGG_LIBRARY ogg)
set(OGG_FOUND TRUE)

# Fetch Vorbis
FetchContent_Declare(
    vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG        v1.3.7
)
FetchContent_MakeAvailable(vorbis)

# Fetch Opus
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.4
)
FetchContent_MakeAvailable(opus)

# Fetch Ableton Link (header-only CMake integration, with asio-standalone submodule)
FetchContent_Declare(
    abletonlink
    GIT_REPOSITORY https://github.com/Ableton/link.git
    GIT_TAG        Link-4.0
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES modules/asio-standalone
    GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_GetProperties(abletonlink)
if (NOT abletonlink_POPULATED)
    FetchContent_Populate(abletonlink)
endif()

include("${abletonlink_SOURCE_DIR}/AbletonLinkConfig.cmake")

set(NINJAM_PLUGIN_FORMATS VST3 Standalone)

if (APPLE)
    list(APPEND NINJAM_PLUGIN_FORMATS AU)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND NINJAM_PLUGIN_FORMATS LV2)
endif()

# Define the plugin
juce_add_plugin(NINJAM_VST3
    VERSION ${NINJAM_PLUGIN_VERSION}
    BUILD_VERSION ${NINJAM_DISPLAY_VERSION}
    COMPANY_NAME "${NINJAM_VENDOR_NAME}"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Juce
    PLUGIN_CODE Njv3
    BUNDLE_ID "com.AMP.NINJAMplus"
    LV2URI "urn:amp:ninjamplus"
    FORMATS ${NINJAM_PLUGIN_FORMATS}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/icon.png"
    PRODUCT_NAME "${NINJAM_BRAND_NAME}"
)

set_target_properties(NINJAM_VST3 PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${NINJAM_OUTPUT_ROOT}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${NINJAM_OUTPUT_ROOT}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${NINJAM_OUTPUT_ROOT}/$<CONFIG>")

# Generate JuceHeader.h
juce_generate_juce_header(NINJAM_VST3)

# WDL and NINJAM sources
set(NINJAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ninjam")
set(WDL_ROOT "${NINJAM_ROOT}/WDL")

# Define sources
target_sources(NINJAM_VST3 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/SolititoChordModel.cpp
    Source/SolititoChordModel.h
    Source/StandaloneApp.cpp
    Source/ZapVideoCodec.cpp
    Source/ZapVideoCodec.h
    Source/AutoTune.h
    Source/SshTunnel.h
    Source/SshTunnel.cpp
    Source/SessionRecorder.h
    Source/SessionRecorder.cpp
    
    # NINJAM Core
    ${NINJAM_ROOT}/ninjam/njclient.cpp
    ${NINJAM_ROOT}/ninjam/netmsg.cpp
    ${NINJAM_ROOT}/ninjam/mpb.cpp
    ${NINJAM_ROOT}/ninjam/njmisc.cpp
    
    # WDL sources needed by NINJAM
    ${WDL_ROOT}/jnetlib/asyncdns.cpp
    ${WDL_ROOT}/jnetlib/connection.cpp
    ${WDL_ROOT}/jnetlib/httpget.cpp
    ${WDL_ROOT}/jnetlib/util.cpp
    ${WDL_ROOT}/jnetlib/listen.cpp
    ${WDL_ROOT}/jnetlib/webserver.cpp
    
    ${WDL_ROOT}/sha.cpp
    ${WDL_ROOT}/rng.cpp
    ${WDL_ROOT}/resample.cpp # Might be needed
    ${WDL_ROOT}/fft.c
    ${WDL_ROOT}/lice/lice.cpp
    ${WDL_ROOT}/lice/lice_gif.cpp
    ${WDL_ROOT}/giflib/dgif_lib.c
    ${WDL_ROOT}/giflib/gifalloc.c
    third_party/chord-detector/src/Chromagram.cpp
    third_party/chord-detector/src/ChordDetector.cpp
    third_party/chord-detector/libs/kiss_fft130/kiss_fft.c

    # Rubber Band Library (single-file compilation unit for pitch shifting / auto-tune)
    third_party/rubberband/single/RubberBandSingle.cpp

    # LibrosaCpp (olilarkin/librosa.cpp) — tempo estimation and beat tracking
    third_party/librosa-cpp/vendor/incbeta/incbeta.c
    third_party/librosa-cpp/src/internal/fft_pocketfft.cpp
    third_party/librosa-cpp/src/util/utils.cpp
    third_party/librosa-cpp/src/util/exceptions.cpp
    third_party/librosa-cpp/src/core/convert.cpp
    third_party/librosa-cpp/src/core/audio.cpp
    third_party/librosa-cpp/src/core/spectrum.cpp
    third_party/librosa-cpp/src/core/pitch.cpp
    third_party/librosa-cpp/src/core/harmonic.cpp
    third_party/librosa-cpp/src/core/constantq.cpp
    third_party/librosa-cpp/src/core/notation.cpp
    third_party/librosa-cpp/src/core/intervals.cpp
    third_party/librosa-cpp/src/filters.cpp
    third_party/librosa-cpp/src/sequence.cpp
    third_party/librosa-cpp/src/feature/spectral.cpp
    third_party/librosa-cpp/src/feature/rhythm.cpp
    third_party/librosa-cpp/src/feature/inverse.cpp
    third_party/librosa-cpp/src/onset.cpp
    third_party/librosa-cpp/src/beat.cpp
    third_party/librosa-cpp/src/decompose.cpp
    third_party/librosa-cpp/src/segment.cpp
    third_party/librosa-cpp/src/effects.cpp
)

# Rubber Band needs NOMINMAX on Windows to avoid std::min/max macro conflicts
if (WIN32)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rubberband/single/RubberBandSingle.cpp"
        PROPERTIES COMPILE_DEFINITIONS NOMINMAX
    )
    # LibrosaCpp / Eigen also need NOMINMAX and _USE_MATH_DEFINES on Windows
    file(GLOB LIBROSA_CPP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librosa-cpp/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librosa-cpp/src/core/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librosa-cpp/src/feature/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librosa-cpp/src/internal/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/librosa-cpp/src/util/*.cpp"
    )
    foreach(_librosa_src ${LIBROSA_CPP_SOURCES})
        set_source_files_properties(
            "${_librosa_src}"
            PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;_USE_MATH_DEFINES"
        )
    endforeach()
endif()

# Include directories
target_include_directories(NINJAM_VST3 PRIVATE
    Source
    ${NINJAM_ROOT}
    ${NINJAM_ROOT}/ninjam
    ${WDL_ROOT}
    ${ogg_SOURCE_DIR}/include
    ${vorbis_SOURCE_DIR}/include
    ${vorbis_SOURCE_DIR}/lib # for vorbis internal headers if needed
    ${opus_SOURCE_DIR}/include
    third_party/chord-detector/src
    third_party/chord-detector/libs/kiss_fft130
    third_party/chord-detector
    third_party/rubberband
    third_party/librosa-cpp/include
    third_party/librosa-cpp/src
    third_party/librosa-cpp/vendor
    third_party/librosa-cpp/modules/eigen
    third_party/librosa-cpp/modules/pocketfft
    $<$<BOOL:${WIN32}>:${asiosdk_SOURCE_DIR}/common>
    $<$<BOOL:${WIN32}>:${asiosdk_SOURCE_DIR}/host>
)

# Optional H.264 decode backends. MJPEG decode is always available through JUCE.
if (WIN32)
    target_sources(NINJAM_VST3 PRIVATE
        Source/ProVideoDecoder.h
        Source/ProVideoDecoder_MF.cpp
    )
    target_compile_definitions(NINJAM_VST3 PRIVATE
        NINJAMPLUS_HAS_PROVIDEO=1
        NINJAMPLUS_HAS_H264_DECODE=1)
    target_link_libraries(NINJAM_VST3 PRIVATE Mfplat Mfreadwrite Mfuuid wmcodecdspuuid)
elseif (APPLE)
    target_sources(NINJAM_VST3 PRIVATE
        Source/ProVideoDecoder.h
        Source/ProVideoDecoder_VT.mm
    )
    target_compile_definitions(NINJAM_VST3 PRIVATE
        NINJAMPLUS_HAS_PROVIDEO=1
        NINJAMPLUS_HAS_H264_DECODE=1)
    target_link_libraries(NINJAM_VST3 PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation")
else()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET
            gstreamer-1.0
            gstreamer-app-1.0
            gstreamer-video-1.0)
    endif()

    if (TARGET PkgConfig::GSTREAMER)
        target_sources(NINJAM_VST3 PRIVATE
            Source/ProVideoDecoder.h
            Source/ProVideoDecoder_GStreamer.cpp
        )
        target_link_libraries(NINJAM_VST3 PRIVATE PkgConfig::GSTREAMER)
        target_compile_definitions(NINJAM_VST3 PRIVATE
            NINJAMPLUS_HAS_PROVIDEO=1
            NINJAMPLUS_HAS_H264_DECODE=1)
    else()
        target_compile_definitions(NINJAM_VST3 PRIVATE
            NINJAMPLUS_HAS_PROVIDEO=0
            NINJAMPLUS_HAS_H264_DECODE=0)
    endif()
endif()

# LibrosaCpp is always available (bundled with pocketfft backend)
target_compile_definitions(NINJAM_VST3 PRIVATE NINJAMPLUS_HAS_LIBROSA=1)

set(NINJAMPLUS_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
set(NINJAMPLUS_SOLITITO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/solitito-ai")
set(NINJAMPLUS_ONNX_CHORDS_AVAILABLE FALSE)
if (WIN32
    AND EXISTS "${NINJAMPLUS_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h"
    AND EXISTS "${NINJAMPLUS_ONNXRUNTIME_ROOT}/bin/win-x64/onnxruntime.dll"
    AND EXISTS "${NINJAMPLUS_SOLITITO_ROOT}/chord_model_v31_16k.onnx"
    AND EXISTS "${NINJAMPLUS_SOLITITO_ROOT}/dsp_weights_v31_16k.bin")
    set(NINJAMPLUS_ONNX_CHORDS_AVAILABLE TRUE)
    target_include_directories(NINJAM_VST3 PRIVATE "${NINJAMPLUS_ONNXRUNTIME_ROOT}/include")
    target_compile_definitions(NINJAM_VST3 PRIVATE
        NINJAMPLUS_HAS_ONNX_CHORDS=1
        NINJAMPLUS_SOLITITO_ASSET_DIR="${NINJAMPLUS_SOLITITO_ROOT}")
else()
    target_compile_definitions(NINJAM_VST3 PRIVATE NINJAMPLUS_HAS_ONNX_CHORDS=0)
endif()
if (MSVC)
    target_compile_options(NINJAM_VST3 PRIVATE /bigobj /FS)
endif()

# Link libraries
target_link_libraries(NINJAM_VST3 PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_video
    juce::juce_dsp
    juce::juce_osc
    Ableton::Link
    ogg
    vorbis
    vorbisenc
    vorbisfile
    opus
)

# Compiler definitions
target_compile_definitions(NINJAM_VST3 PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CAMERA=1
    JUCE_USE_CURL=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    NINJAM_DISPLAY_VERSION=\"${NINJAM_DISPLAY_VERSION}\"
    NINJAMPLUS_HAS_LIBVPX=0
    NINJAMPLUS_FORCE_INTERNAL_VORBIS
    WDL_NO_DEFINE_MINMAX
    _LICE_NO_SYSBITMAPS_
    USE_KISS_FFT
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    JUCE_VST3_CAN_REPLACE_VST2=0
    $<$<BOOL:${WIN32}>:JUCE_ASIO=1>
)

if (WIN32)
    target_sources(NINJAM_VST3 PRIVATE ${WDL_ROOT}/win32_utf8.c)
    target_link_libraries(NINJAM_VST3 PRIVATE ws2_32 winmm)
endif()

if (TARGET NINJAM_VST3_Standalone)
    # Use a custom Info.plist for the Standalone app so macOS privacy keys are present.
    # This ensures the built .app contains NSMicrophoneUsageDescription and macOS will
    # prompt for microphone access when the app opens audio input.
    set(INFOPLIST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Info-Standalone.plist")
    if (EXISTS "${INFOPLIST_FILE}")
        set_target_properties(NINJAM_VST3_Standalone PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${INFOPLIST_FILE}")
    else()
        message(STATUS "Info-Standalone.plist not found at ${INFOPLIST_FILE}; not setting bundle plist")
    endif()
    add_custom_target(NINJAM_VST3_RemoveStandaloneLegacyWebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_Standalone NINJAM_VST3_RemoveStandaloneLegacyWebAssets)
endif()

if (TARGET NINJAM_VST3_VST3)
    add_custom_target(NINJAM_VST3_RemoveVst3LegacyWebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_VST3 NINJAM_VST3_RemoveVst3LegacyWebAssets)
endif()

if (TARGET NINJAM_VST3_LV2)
    add_custom_target(NINJAM_VST3_RemoveLv2LegacyWebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_LV2 NINJAM_VST3_RemoveLv2LegacyWebAssets)
endif()

if (NINJAMPLUS_ONNX_CHORDS_AVAILABLE)
    set(NINJAMPLUS_ONNXRUNTIME_DLL "${NINJAMPLUS_ONNXRUNTIME_ROOT}/bin/win-x64/onnxruntime.dll")
    set(NINJAMPLUS_SOLITITO_MODEL "${NINJAMPLUS_SOLITITO_ROOT}/chord_model_v31_16k.onnx")
    set(NINJAMPLUS_SOLITITO_WEIGHTS "${NINJAMPLUS_SOLITITO_ROOT}/dsp_weights_v31_16k.bin")

    if (TARGET juce_vst3_helper)
        add_custom_command(TARGET juce_vst3_helper POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:juce_vst3_helper>/ninjamplus_onnxruntime.dll"
            VERBATIM)
    endif()

    foreach(_ninjam_runtime_target NINJAM_VST3_Standalone NINJAM_VST3_VST3 NINJAM_VST3_LV2 NINJAM_VST3_AU)
        if (TARGET ${_ninjam_runtime_target})
            add_custom_command(TARGET ${_ninjam_runtime_target} PRE_LINK
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai"
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/ninjamplus_onnxruntime.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_MODEL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai/chord_model_v31_16k.onnx"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_WEIGHTS}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai/dsp_weights_v31_16k.bin"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_MODEL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai/chord_model_v31_16k.onnx"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_WEIGHTS}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai/dsp_weights_v31_16k.bin"
                VERBATIM)
            add_custom_command(TARGET ${_ninjam_runtime_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai"
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/ninjamplus_onnxruntime.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_MODEL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai/chord_model_v31_16k.onnx"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_WEIGHTS}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/solitito-ai/dsp_weights_v31_16k.bin"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_MODEL}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai/chord_model_v31_16k.onnx"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NINJAMPLUS_SOLITITO_WEIGHTS}" "$<TARGET_FILE_DIR:${_ninjam_runtime_target}>/../Resources/solitito-ai/dsp_weights_v31_16k.bin"
                VERBATIM)
        endif()
    endforeach()
endif()
