cmake_minimum_required(VERSION 3.22)
project(FlipKit64 VERSION 0.9.10)

# Path to JUCE 7 (tested with 7.0.12). Default: a JUCE folder next to this one.
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE source tree")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

juce_add_plugin(FlipKit64
    VERSION 0.9.10
    COMPANY_NAME "seandrethegiant"
    BUNDLE_ID com.seandrethegiant.flipkit64
    PRODUCT_NAME "FlipKit64"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    PLUGIN_MANUFACTURER_CODE Sean
    PLUGIN_CODE PdRm
    VST3_CATEGORIES Instrument Drum Sampler
    FORMATS VST3 Standalone
    VST3_AUTO_MANIFEST FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(FlipKit64 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SamplerPage.cpp
    Source/SequencerPage.cpp
    Source/KeysRoll.cpp Source/EnvelopeEditor.cpp Source/Lights.cpp Source/KitFill.cpp Source/SampleSearch.cpp
    Source/Network.cpp
    Source/NetBrowser.cpp
    Source/PreviewStrip.cpp)

target_compile_features(FlipKit64 PRIVATE cxx_std_17)

# Signalsmith Stretch (MIT, header-only) + its FFT helper, vendored under ThirdParty.
target_include_directories(FlipKit64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

target_compile_definitions(FlipKit64 PUBLIC
    FLIPKIT_VERSION_LABEL="0.9.10 beta"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(FlipKit64
    PRIVATE juce::juce_audio_utils juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Network locations (SMB shares) via libsmb2 (LGPL-2.1+). Windows reaches shares natively (\\PC\share),
# so it's only built elsewhere (Linux, ChromeOS, and later Android).
if (WIN32)
    set(FLIPKIT_NETWORK_DEFAULT OFF)
else()
    set(FLIPKIT_NETWORK_DEFAULT ON)
endif()
option(FLIPKIT_NETWORK "Network locations (SMB) in the sample browser" ${FLIPKIT_NETWORK_DEFAULT})
if (FLIPKIT_NETWORK)
    include(FetchContent)
    # Plain NTLM login only: no Kerberos, QUIC, RDMA, DCE/RPC or command-line tools.
    foreach (opt ENABLE_EXAMPLES ENABLE_UTILS ENABLE_LIBKRB5 ENABLE_GSSAPI ENABLE_LIBQUIC ENABLE_RDMA ENABLE_LIBDCERPC)
        set (${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_LibKrb5 ON)
    # Pinned to a 2026 commit: release v6.0.0 crashes listing folders that contain non-ASCII names.
    FetchContent_Declare(libsmb2
        GIT_REPOSITORY https://github.com/sahlberg/libsmb2.git
        GIT_TAG        727feaaa6d0f4fed1bae57745fbc2c5db9a30e64)
    FetchContent_MakeAvailable(libsmb2)
    set_target_properties(smb2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(FlipKit64 PRIVATE ${libsmb2_SOURCE_DIR}/include)
    target_link_libraries(FlipKit64 PRIVATE smb2)
    target_compile_definitions(FlipKit64 PUBLIC FLIPKIT_NETWORK=1)
endif()

# Stem separation (drums / bass / vocals / other) with HTDemucs through ONNX Runtime.
# Only the ONNX Runtime *headers* are needed to build: the runtime library itself is loaded at run
# time (see Source/StemSeparator.h), so a build without it still runs and just hides the feature.
# On by default for Windows (the release zip ships onnxruntime.dll); off elsewhere (the Chromebook is ARM and too slow).
if (WIN32)
    set(FLIPKIT_STEMS_DEFAULT ON)
else()
    set(FLIPKIT_STEMS_DEFAULT OFF)
endif()
option(FLIPKIT_STEMS "Stem separation (needs the ONNX Runtime library at run time)" ${FLIPKIT_STEMS_DEFAULT})
if (FLIPKIT_STEMS)
    include(FetchContent)
    FetchContent_Declare(ort_headers
        URL https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-x64-1.22.0.tgz
        URL_HASH SHA256=8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3)
    FetchContent_MakeAvailable(ort_headers)
    target_sources(FlipKit64 PRIVATE Source/StemSeparator.cpp Source/StemJob.cpp)
    target_include_directories(FlipKit64 PRIVATE ${ort_headers_SOURCE_DIR}/include)
    target_compile_definitions(FlipKit64 PUBLIC FLIPKIT_STEMS=1)
endif()

if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(FlipKit64 PRIVATE -static -static-libgcc -static-libstdc++)
endif()

option(PADROMPLER_TESTS "Build offline audio tests" OFF)
if (PADROMPLER_TESTS)
    juce_add_console_app(rompler_test)
    target_sources(rompler_test PRIVATE tests/test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SamplerPage.cpp Source/SequencerPage.cpp Source/KeysRoll.cpp Source/EnvelopeEditor.cpp Source/Lights.cpp Source/KitFill.cpp Source/SampleSearch.cpp Source/Network.cpp Source/NetBrowser.cpp Source/PreviewStrip.cpp)
    target_compile_definitions(rompler_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1 JUCE_USE_MP3AUDIOFORMAT=1 JucePlugin_Name="FlipKit64" FLIPKIT_VERSION_LABEL="0.9.10 beta" FLIPKIT_DEV=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_include_directories(rompler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)
    target_link_libraries(rompler_test PRIVATE juce::juce_audio_utils juce::juce_audio_formats juce::juce_recommended_config_flags)
    if (FLIPKIT_STEMS)
        target_sources(rompler_test PRIVATE Source/StemSeparator.cpp Source/StemJob.cpp)
        target_include_directories(rompler_test PRIVATE ${ort_headers_SOURCE_DIR}/include)
        target_compile_definitions(rompler_test PRIVATE FLIPKIT_STEMS=1)
    endif()
    if (FLIPKIT_NETWORK)
        target_include_directories(rompler_test PRIVATE ${libsmb2_SOURCE_DIR}/include)
        target_link_libraries(rompler_test PRIVATE smb2)
        target_compile_definitions(rompler_test PRIVATE FLIPKIT_NETWORK=1)
    endif()
endif()

# Offline test for the stem separator's DSP and chunk stitching (see tests/stems_test.cpp).
if (FLIPKIT_STEMS AND PADROMPLER_TESTS)
    juce_add_console_app(stems_test)
    target_sources(stems_test PRIVATE tests/stems_test.cpp Source/StemSeparator.cpp)
    target_include_directories(stems_test PRIVATE ${ort_headers_SOURCE_DIR}/include)
    target_compile_definitions(stems_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(stems_test PRIVATE juce::juce_audio_utils juce::juce_audio_formats juce::juce_recommended_config_flags)
endif()
