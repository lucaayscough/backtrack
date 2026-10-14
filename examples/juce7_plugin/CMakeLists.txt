cmake_minimum_required(VERSION 3.22)

# JUCE 7 still calls CGWindowListCreateImage, which macOS 15+ SDKs make
# unavailable when targeting macOS 15 or later. The environment variable also
# reaches JUCE's separately configured juceaide build.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(APPLE)
  set(ENV{MACOSX_DEPLOYMENT_TARGET} "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# A minimal plugin that integrates Backtrack on JUCE 7. Configuring it also
# builds Backtrack's own tests and demo against the same JUCE version.
project(backtrack_juce7_plugin VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 7.0.12
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

enable_testing()
set(LA_BACKTRACK_BUILD_TESTS ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. backtrack)

set(formats VST3 Standalone)
if(APPLE)
  list(APPEND formats AU)
endif()

juce_add_plugin(BacktrackJuce7Plugin
  COMPANY_NAME "la"
  PRODUCT_NAME "Backtrack JUCE7 Example"
  PLUGIN_MANUFACTURER_CODE Labk
  PLUGIN_CODE Btk7
  FORMATS ${formats}
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(BacktrackJuce7Plugin PRIVATE plugin.cpp)
target_compile_definitions(BacktrackJuce7Plugin PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(BacktrackJuce7Plugin
  PRIVATE
    la::backtrack
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
