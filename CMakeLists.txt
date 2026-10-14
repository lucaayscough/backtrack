cmake_minimum_required(VERSION 3.22)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(la_backtrack VERSION 0.1.0 LANGUAGES C CXX)

# Reuse the parent project's JUCE when it already provides one.
if(NOT TARGET juce::juce_core)
  include(FetchContent)
  FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

add_library(la_backtrack INTERFACE)
add_library(la::backtrack ALIAS la_backtrack)

target_compile_features(la_backtrack INTERFACE cxx_std_20)
target_include_directories(la_backtrack INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(la_backtrack INTERFACE
  juce::juce_audio_basics
  juce::juce_gui_extra)

option(LA_BACKTRACK_BUILD_TESTS "Build Backtrack tests" ${PROJECT_IS_TOP_LEVEL})
if(LA_BACKTRACK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
