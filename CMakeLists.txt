cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(MSVC)
    # Static runtime: one portable EXE without the Visual C++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HTH_VERSION LIMIT_COUNT 1)
string(STRIP "${HTH_VERSION}" HTH_VERSION)
project(HeadTrackingHost VERSION ${HTH_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
# Fix for VST3 shell plugins such as Waves' WaveShell (see the script).
execute_process(COMMAND ${CMAKE_COMMAND} -DJUCE_SOURCE=${juce_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceVst3ShellIndex.cmake
                COMMAND_ERROR_IS_FATAL ANY)

juce_add_gui_app(HeadTrackingHost
    PRODUCT_NAME "Head Tracking Host"
    COMPANY_NAME "lumen-tools"
    BUNDLE_ID io.github.lumen-tools.head-tracking-host
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Head Tracking Host needs audio input to process the sound coming from your virtual audio device.")

target_sources(HeadTrackingHost PRIVATE Source/Main.cpp)
if(UNIX AND NOT APPLE)
    target_link_libraries(HeadTrackingHost PRIVATE curl) # Linux test builds: downloads via libcurl
endif()
if(APPLE)
    target_sources(HeadTrackingHost PRIVATE Source/MicrophonePermission.mm)
    target_link_libraries(HeadTrackingHost PRIVATE "-framework AVFoundation")
endif()

target_compile_definitions(HeadTrackingHost PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:HeadTrackingHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:HeadTrackingHost,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=$<IF:$<PLATFORM_ID:Linux>,1,0>
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# License texts shown in the app ("About & licenses"): our license plus the licenses of the
# third-party code that JUCE compiles into the program.
set(HTH_NOTICES "${CMAKE_CURRENT_BINARY_DIR}/ThirdPartyNotices.txt")
file(WRITE "${HTH_NOTICES}" "")
foreach(entry
        "JUCE framework (used under the AGPLv3)|LICENSE.md"
        "VST3 SDK (MIT)|modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
        "ASIO SDK (used under the GPLv3)|modules/juce_audio_devices/native/asio/LICENSE.txt"
        "FLAC (BSD)|modules/juce_audio_formats/codecs/flac/Flac Licence.txt"
        "Ogg Vorbis (BSD)|modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt"
        "HarfBuzz (Old MIT)|modules/juce_graphics/fonts/harfbuzz/COPYING"
        "SheenBidi (Apache 2.0)|modules/juce_graphics/unicode/sheenbidi/LICENSE"
        "libpng (libpng/zlib licence)|modules/juce_graphics/image_formats/pnglib/LICENSE"
        "zlib (zlib licence)|modules/juce_core/zip/zlib/README"
        "Independent JPEG Group's jpeglib (IJG licence)|modules/juce_graphics/image_formats/jpglib/README")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 title)
    list(GET parts 1 path)
    file(READ "${juce_SOURCE_DIR}/${path}" text)
    file(APPEND "${HTH_NOTICES}" "\n==============================================================================\n${title}\n==============================================================================\n\n${text}\n")
endforeach()
juce_add_binary_data(HeadTrackingHostData SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${HTH_NOTICES}")

target_link_libraries(HeadTrackingHost
    PRIVATE
        HeadTrackingHostData
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
