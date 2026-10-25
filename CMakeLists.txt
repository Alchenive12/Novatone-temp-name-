cmake_minimum_required(VERSION 3.24)
project(NovaTone VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- JUCE -------------------------------------------------------------------
# Pass -DJUCE_DIR=/path/to/JUCE to use a local checkout instead of fetching.
if (DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---- Signalsmith Stretch (formant / Alter shifter) --------------------------
# Use Signalsmith's portable FFT on every platform: its optional Apple
# Accelerate backend broke the macOS build.
set(SIGNALSMITH_USE_ACCELERATE OFF CACHE BOOL "" FORCE)

if (DEFINED SIGNALSMITH_DIR)
    add_subdirectory(${SIGNALSMITH_DIR} signalsmith-stretch EXCLUDE_FROM_ALL)
    set(SIGNALSMITH_SRC ${SIGNALSMITH_DIR})
else()
    FetchContent_Declare(signalsmith-stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG        1.4.0
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(signalsmith-stretch)
    set(SIGNALSMITH_SRC ${signalsmith-stretch_SOURCE_DIR})
endif()

# Off by default so CI never tries to write into system plugin folders.
option(NOVATONE_COPY_AFTER_BUILD "Copy plugin to the system plugin folder after building" OFF)

juce_add_plugin(NovaTone
    COMPANY_NAME                 "Alchenive"
    IS_SYNTH                     FALSE
    NEEDS_MIDI_INPUT             TRUE
    NEEDS_MIDI_OUTPUT            TRUE   # processor's producesMidi() returns true
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  TRUE
    COPY_PLUGIN_AFTER_BUILD      ${NOVATONE_COPY_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE     Alch
    PLUGIN_CODE                  NvTn
    FORMATS                      VST3 Standalone
    PRODUCT_NAME                 "NovaTone")

# Several GUI headers include <JuceHeader.h>.
juce_generate_juce_header(NovaTone)

# Embedded UI font (Inter, SIL Open Font License - see Resources/Fonts).
juce_add_binary_data(NovaToneFonts
    HEADER_NAME NovaToneFonts.h
    NAMESPACE   NovaToneFonts
    SOURCES     Resources/Fonts/Inter-Regular.ttf Resources/Fonts/Inter-SemiBold.ttf)
set_target_properties(NovaToneFonts PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(NovaTone PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DSP/LookaheadBuffer.cpp
    Source/DSP/MPMDetector.cpp
    Source/DSP/Quantizer.cpp
    Source/DSP/SignalsmithShifter.cpp
    Source/DSP/SmoothingFilter.cpp
    Source/DSP/VoicingGate.cpp
    Source/GUI/ModuleTabs.cpp
    Source/GUI/PitchTraceComponent.cpp
    Source/GUI/TuneTab.cpp)

target_include_directories(NovaTone PRIVATE
    Source
    Source/DSP
    Source/GUI
    ${SIGNALSMITH_SRC})

target_compile_definitions(NovaTone PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(NovaTone
    PRIVATE
        signalsmith-stretch
        NovaToneFonts
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Dev-only: headless editor snapshot tool (never built in CI) ------------
option(NOVATONE_BUILD_SNAPSHOT "Build the NovaToneSnapshot UI preview tool" OFF)
if (NOVATONE_BUILD_SNAPSHOT)
    juce_add_gui_app(NovaToneSnapshot PRODUCT_NAME "NovaToneSnapshot" VERSION ${PROJECT_VERSION})
    juce_generate_juce_header(NovaToneSnapshot)
    get_target_property(_nt_sources NovaTone SOURCES)
    list(FILTER _nt_sources INCLUDE REGEX "^Source/")
    target_sources(NovaToneSnapshot PRIVATE tools/Snapshot.cpp ${_nt_sources})
    target_include_directories(NovaToneSnapshot PRIVATE Source Source/DSP Source/GUI ${SIGNALSMITH_SRC})
    target_compile_definitions(NovaToneSnapshot PRIVATE
        JucePlugin_Name="NovaTone" JucePlugin_IsSynth=0 JucePlugin_IsMidiEffect=0
        JucePlugin_ProducesMidiOutput=1 JucePlugin_WantsMidiInput=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(NovaToneSnapshot PRIVATE signalsmith-stretch NovaToneFonts
        juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats
        juce::juce_dsp juce::juce_gui_basics juce::juce_recommended_config_flags)
endif()

# ---- Dev-only: polyphonic detection / editing benchmark (never built in CI) --
option(NOVATONE_BUILD_TOOLS "Build dev benchmarks (NovaTonePolyBench)" OFF)
if (NOVATONE_BUILD_TOOLS)
    juce_add_console_app(NovaTonePolyBench PRODUCT_NAME "NovaTonePolyBench" VERSION ${PROJECT_VERSION})
    juce_generate_juce_header(NovaTonePolyBench)
    target_sources(NovaTonePolyBench PRIVATE tools/PolyBench.cpp Source/Poly/PolyAnalyzer.cpp
        Source/DSP/LookaheadBuffer.cpp Source/DSP/MPMDetector.cpp Source/DSP/Quantizer.cpp
        Source/DSP/SmoothingFilter.cpp Source/DSP/VoicingGate.cpp)
    target_include_directories(NovaTonePolyBench PRIVATE Source Source/DSP Source/GUI ${SIGNALSMITH_SRC})
    target_compile_definitions(NovaTonePolyBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(NovaTonePolyBench PRIVATE juce::juce_dsp juce::juce_audio_basics juce::juce_gui_basics
        juce::juce_recommended_config_flags)
endif()
