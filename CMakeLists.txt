cmake_minimum_required(VERSION 3.22)

# Fix for old CMake policies in dependencies (ogg/vorbis)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(NINJAM_VERSION_FALLBACK "0.0.0")
set(NINJAM_VERSION_TAG "")
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
    GIT_TAG        8.0.0
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

# Fetch signalsmith-stretch from its upstream repository so CI and local builds
# always obtain the header/source directly during CMake configure.
FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/signalsmith-audio/signalsmith-stretch.git
    GIT_TAG        57b93f4e9206a089a45387eaa39bdc9f310d3308
)
FetchContent_GetProperties(signalsmith_stretch)
if (NOT signalsmith_stretch_POPULATED)
    FetchContent_Populate(signalsmith_stretch)
endif()

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
    Source/StandaloneApp.cpp
    Source/ZapVideoCodec.cpp
    Source/ZapVideoCodec.h
    # ProVideoCodec is optional (FFmpeg-based). Do not add unconditionally.
    
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
)

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
    ${signalsmith_stretch_SOURCE_DIR}/include
    third_party/signalsmith-linear/include
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

set(NINJAMPLUS_LOCAL_AUBIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/local_deps/aubio-install")
find_path(AUBIO_INCLUDE_DIR
    NAMES aubio.h aubio/aubio.h
    HINTS "${NINJAMPLUS_LOCAL_AUBIO_ROOT}/include")
find_library(AUBIO_LIBRARY
    NAMES aubio libaubio
    HINTS "${NINJAMPLUS_LOCAL_AUBIO_ROOT}/lib")
if (AUBIO_INCLUDE_DIR AND AUBIO_LIBRARY)
    target_include_directories(NINJAM_VST3 PRIVATE ${AUBIO_INCLUDE_DIR})
    target_link_libraries(NINJAM_VST3 PRIVATE ${AUBIO_LIBRARY})
    target_compile_definitions(NINJAM_VST3 PRIVATE NINJAMPLUS_HAS_AUBIO=1)
else()
    target_compile_definitions(NINJAM_VST3 PRIVATE NINJAMPLUS_HAS_AUBIO=0)
endif()

if (MSVC)
    target_compile_options(NINJAM_VST3 PRIVATE /bigobj)
endif()

# Link libraries
target_link_libraries(NINJAM_VST3 PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
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
    add_custom_target(NINJAM_VST3_SyncWebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/index.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/app.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client/app.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/icon.png"
            "$<TARGET_FILE_DIR:NINJAM_VST3_Standalone>/advanced-vdo-client/icon.png"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_Standalone NINJAM_VST3_SyncWebAssets)
endif()

if (TARGET NINJAM_VST3_VST3)
    add_custom_target(NINJAM_VST3_SyncVst3WebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/index.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/app.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client/app.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/icon.png"
            "$<TARGET_FILE_DIR:NINJAM_VST3_VST3>/../Resources/advanced-vdo-client/icon.png"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_VST3 NINJAM_VST3_SyncVst3WebAssets)
endif()

if (TARGET NINJAM_VST3_LV2)
    add_custom_target(NINJAM_VST3_SyncLv2WebAssets
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/index.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/app.html"
            "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client/app.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/advanced-vdo-client/icon.png"
            "$<TARGET_FILE_DIR:NINJAM_VST3_LV2>/advanced-vdo-client/icon.png"
        VERBATIM
    )
    add_dependencies(NINJAM_VST3_LV2 NINJAM_VST3_SyncLv2WebAssets)
endif()
