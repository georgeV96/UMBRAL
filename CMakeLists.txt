cmake_minimum_required(VERSION 3.22)
project(UMBRAL VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(UMBRAL_BUILD_TESTS  "Build the engine tests and the offline render tool (no JUCE needed)" ON)
option(UMBRAL_BUILD_PLUGIN "Build the VST3 plugin (needs JUCE, by default the submodule in third_party/JUCE)" ON)
set(UMBRAL_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to a JUCE 8 checkout")

# ---- DSP core: plain C++17, no JUCE ------------------------------------------------------------------------------------
add_library(umbral_dsp STATIC
    Source/DSP/Fft.h
    Source/DSP/Noise.h
    Source/DSP/UmbralEngine.h
    Source/DSP/UmbralEngine.cpp
    Source/Presets.h)
target_include_directories(umbral_dsp PUBLIC Source)
if(NOT MSVC)
    target_compile_options(umbral_dsp PRIVATE -Wall -Wextra -Wshadow)
endif()

if(UMBRAL_BUILD_TESTS)
    enable_testing()
    add_executable(umbral_tests Tests/UmbralTests.cpp)
    target_link_libraries(umbral_tests PRIVATE umbral_dsp)
    add_test(NAME umbral_engine COMMAND umbral_tests)

    add_executable(umbral_render Tools/Render.cpp)
    target_link_libraries(umbral_render PRIVATE umbral_dsp)
endif()

# ---- VST3 ----------------------------------------------------------------------------------------------------------------
if(UMBRAL_BUILD_PLUGIN)
    if(NOT EXISTS "${UMBRAL_JUCE_DIR}/CMakeLists.txt")
        message(WARNING "JUCE not found in ${UMBRAL_JUCE_DIR}: run 'git submodule update --init' or pass -DUMBRAL_JUCE_DIR=<path>. Skipping the plugin.")
    else()
        # JUCE 8 is AGPLv3 / commercial: decide before distributing.
        add_subdirectory("${UMBRAL_JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce")

        set(UMBRAL_FORMATS VST3)
        if(APPLE)
            list(APPEND UMBRAL_FORMATS AU)
        endif()

        juce_add_plugin(Umbral
            COMPANY_NAME "Lazarus Noise"
            PLUGIN_MANUFACTURER_CODE LzNs
            PLUGIN_CODE Umbr
            FORMATS ${UMBRAL_FORMATS}
            PRODUCT_NAME "Umbral"
            BUNDLE_ID com.lazarusnoise.umbral
            IS_SYNTH FALSE
            NEEDS_MIDI_INPUT FALSE
            NEEDS_MIDI_OUTPUT FALSE
            VST3_CATEGORIES Fx
            AU_MAIN_TYPE kAudioUnitType_Effect
            COPY_PLUGIN_AFTER_BUILD FALSE)

        target_sources(Umbral PRIVATE
            Source/PluginProcessor.h
            Source/PluginProcessor.cpp
            Source/PluginEditor.h
            Source/PluginEditor.cpp)

        target_compile_definitions(Umbral PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0)
        target_compile_options(Umbral PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

        target_link_libraries(Umbral
            PRIVATE
                umbral_dsp
                juce::juce_audio_processors
                juce::juce_gui_basics
            PUBLIC
                juce::juce_recommended_config_flags
                juce::juce_recommended_lto_flags
                juce::juce_recommended_warning_flags)

        # Headless host: instantiates the real AudioProcessor and checks the plugin layer end to end.
        if(UMBRAL_BUILD_TESTS)
            juce_add_console_app(UmbralHostTest PRODUCT_NAME "UmbralHostTest")
            target_sources(UmbralHostTest PRIVATE Tests/PluginHostTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
            target_compile_definitions(UmbralHostTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
            target_link_libraries(UmbralHostTest
                PRIVATE umbral_dsp juce::juce_audio_processors juce::juce_gui_basics
                PUBLIC juce::juce_recommended_config_flags)
            add_test(NAME umbral_plugin_host COMMAND UmbralHostTest)
        endif()
    endif()
endif()
