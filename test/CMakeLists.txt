find_package(Threads REQUIRED)

function(la_backtrack_add_smoke_test name)
  add_executable(${name} smoke.cpp odr.cpp)
  target_link_libraries(${name} PRIVATE la_backtrack Threads::Threads)
  target_compile_definitions(${name} PRIVATE
    LA_BACKTRACK_RESERVE_SECONDS=1
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

la_backtrack_add_smoke_test(la_backtrack_smoke)

option(LA_BACKTRACK_TSAN "Also build the smoke test with ThreadSanitizer" OFF)
if(LA_BACKTRACK_TSAN)
  la_backtrack_add_smoke_test(la_backtrack_smoke_tsan)
  target_compile_options(la_backtrack_smoke_tsan PRIVATE -fsanitize=thread)
  target_link_options(la_backtrack_smoke_tsan PRIVATE -fsanitize=thread)
  # The GUI tests exercise JUCE's message loop rather than Backtrack's threading.
  set_tests_properties(la_backtrack_smoke_tsan PROPERTIES ENVIRONMENT
    "LA_BACKTRACK_SKIP_GUI_TEST=1;TSAN_OPTIONS=halt_on_error=1")
endif()

option(LA_BACKTRACK_BUILD_DEMO "Build the Backtrack desktop demo" ON)
if(LA_BACKTRACK_BUILD_DEMO)
  juce_add_gui_app(la_backtrack_demo
    PRODUCT_NAME "Backtrack Demo"
    BUNDLE_ID "com.la.backtrack-demo")
  target_sources(la_backtrack_demo PRIVATE demo.cpp)
  target_link_libraries(la_backtrack_demo PRIVATE
    la_backtrack
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
  target_compile_definitions(la_backtrack_demo PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:la_backtrack_demo,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
endif()
