cmake_minimum_required(VERSION 3.22)
project(PitchCoach VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")

# Reuse the JUCE checkout from the Symbios project if it is there (no second download).
set(SYMBIOS_JUCE "${CMAKE_CURRENT_SOURCE_DIR}/../symbios/build/_deps/juce-src")
if(EXISTS "${SYMBIOS_JUCE}/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_JUCE)
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${SYMBIOS_JUCE}" CACHE PATH "")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PitchCoach
    COMPANY_NAME "Mikato"
    PRODUCT_NAME "Pitch Coach"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    PLUGIN_MANUFACTURER_CODE Mkto
    PLUGIN_CODE Pch1
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Analyzer
    FORMATS AU VST3 Standalone
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(PitchCoach PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(PitchCoach PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(PitchCoach
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline test: runs a recorded take through the processor and prints the held-note statistics.
juce_add_console_app(PitchCoachCheck PRODUCT_NAME "PitchCoachCheck")
target_sources(PitchCoachCheck PRIVATE Test/CheckTake.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(PitchCoachCheck PRIVATE
    JucePlugin_Name="Pitch Coach" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(PitchCoachCheck PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
