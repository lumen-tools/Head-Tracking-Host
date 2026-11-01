cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(MSVC)
    # Static runtime: one portable EXE without the Visual C++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(HeadTrackingHost VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(HeadTrackingHost
    PRODUCT_NAME "Head Tracking Host"
    COMPANY_NAME "lumen-tools"
    BUNDLE_ID io.github.lumen-tools.head-tracking-host
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Head Tracking Host needs audio input to process the sound coming from your virtual audio device.")

target_sources(HeadTrackingHost PRIVATE Source/Main.cpp)

target_compile_definitions(HeadTrackingHost PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:HeadTrackingHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:HeadTrackingHost,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(HeadTrackingHost
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
