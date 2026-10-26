cmake_minimum_required(VERSION 3.22)
project(FlipKit64 VERSION 0.9.0)

# Path to JUCE 7 (tested with 7.0.12). Default: a JUCE folder next to this one.
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE source tree")
add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

juce_add_plugin(FlipKit64
    VERSION 0.9.0
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
    Source/SequencerPage.cpp)

target_compile_features(FlipKit64 PRIVATE cxx_std_17)

target_compile_definitions(FlipKit64 PUBLIC
    FLIPKIT_VERSION_LABEL="0.9 beta"
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

if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(FlipKit64 PRIVATE -static -static-libgcc -static-libstdc++)
endif()

option(PADROMPLER_TESTS "Build offline audio tests" OFF)
if (PADROMPLER_TESTS)
    juce_add_console_app(rompler_test)
    target_sources(rompler_test PRIVATE tests/test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SamplerPage.cpp Source/SequencerPage.cpp)
    target_compile_definitions(rompler_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1 JUCE_USE_MP3AUDIOFORMAT=1 JucePlugin_Name="FlipKit64" FLIPKIT_VERSION_LABEL="0.9 beta" FLIPKIT_DEV=1)
    target_link_libraries(rompler_test PRIVATE juce::juce_audio_utils juce::juce_audio_formats juce::juce_recommended_config_flags)
endif()
