cmake_minimum_required(VERSION 3.22)
project(PinkFX VERSION 1.0.0)

option(PINKFX_COPY "Copy plugin to the system VST3 folder after build" ON)
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PinkFX
  COMPANY_NAME "YourName"
  PLUGIN_MANUFACTURER_CODE Ynam
  PLUGIN_CODE Pfx1
  FORMATS VST3 Standalone
  PRODUCT_NAME "PinkFX"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  COPY_PLUGIN_AFTER_BUILD ${PINKFX_COPY})

target_sources(PinkFX PRIVATE Source/Plugin.cpp)
target_compile_definitions(PinkFX PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(PinkFX PRIVATE
  juce::juce_audio_utils
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
