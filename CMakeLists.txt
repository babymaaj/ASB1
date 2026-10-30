cmake_minimum_required(VERSION 3.25)
# macOS: universal (arm64 + x86_64) is passed by CI via -DCMAKE_OSX_ARCHITECTURES; minimum macOS 10.15
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(ASB VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  enable_language(RC)
endif()
if(APPLE)
  enable_language(OBJCXX)
endif()
if(MSVC)  # one CRT everywhere (static, no VC++ redistributable needed on the user's machine)
  set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)   # sources are UTF-8 (MSVC otherwise assumes the system code page)
  # PDB symbols for Release too, so crashes (CI, users) give readable stack traces. Doesn't change the code.
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

option(ASB_BUILD_VST3  "Build the VST3 plugin (needs VST3 SDK)" ON)
option(ASB_BUILD_TESTS "Build core tests/benchmarks" ON)
option(ASB_RUN_VALIDATOR "Run the Steinberg validator after linking the plugin" ON)  # OFF for cross-builds
set(ASB_VST3_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk" CACHE PATH "Path to VST3 SDK checkout")

# ---- platform-independent core + engine (no VST3 dependency) ----
if(WIN32)
  set(ASB_ENGINE_SRC src/engine/StreamEngine_win.cpp)
elseif(APPLE)
  set(ASB_ENGINE_SRC src/engine/StreamEngine_mac.cpp)
else()
  set(ASB_ENGINE_SRC src/engine/StreamEngine_stub.cpp)
endif()
add_library(asb_engine STATIC ${ASB_ENGINE_SRC})
set_target_properties(asb_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(asb_engine PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(asb_engine PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(asb_engine PUBLIC ole32 avrt uuid)
elseif(APPLE)
  target_link_libraries(asb_engine PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()
if(MSVC)
  target_compile_options(asb_engine PRIVATE /W4 /permissive-)
  target_compile_definitions(asb_engine PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(asb_engine PRIVATE -Wall -Wextra)
endif()

if(ASB_BUILD_TESTS)
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE asb_engine)
  enable_testing()
  add_test(NAME core_tests COMMAND core_tests)
  add_executable(asb_probe tests/asb_probe.cpp)
  target_link_libraries(asb_probe PRIVATE asb_engine)
endif()

# ---- VST3 ----
if(ASB_BUILD_VST3)
  if(NOT EXISTS "${ASB_VST3_SDK_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "VST3 SDK not found at ${ASB_VST3_SDK_PATH}. See BUILD_INSTRUCTIONS.md (or -DASB_BUILD_VST3=OFF).")
  endif()
  set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)   # no heavy UI framework
  set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)      # we install manually / via script
  set(SMTG_RUN_VST_VALIDATOR ${ASB_RUN_VALIDATOR} CACHE BOOL "" FORCE)
  add_subdirectory("${ASB_VST3_SDK_PATH}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
  smtg_enable_vst3_sdk()

  if(WIN32)
    set(ASB_EDITOR_SRC src/ui/EditorCore.cpp src/vst/Editor_win.cpp src/vst/Update_win.cpp)
  elseif(APPLE)
    set(ASB_EDITOR_SRC src/ui/EditorCore.cpp src/vst/Editor_mac.mm src/vst/Update_mac.mm)
    set_source_files_properties(src/vst/Editor_mac.mm src/vst/Update_mac.mm tests/editor_preview_mac.mm
                                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  else()
    set(ASB_EDITOR_SRC src/vst/Editor_none.cpp src/vst/Update_none.cpp)
  endif()
  set(SMTG_PACKAGE_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/asb.ico")   # bundle folder icon
  if(WIN32)
    list(APPEND ASB_EDITOR_SRC src/vst/asb.rc)                                # file properties + icon
  endif()
  smtg_add_vst3plugin(ASB
    src/vst/Entry.cpp src/vst/Processor.cpp src/vst/Controller.cpp ${ASB_EDITOR_SRC})
  target_include_directories(ASB PRIVATE src)
  target_link_libraries(ASB PRIVATE sdk asb_engine)
  if(WIN32)
    target_link_libraries(ASB PRIVATE gdiplus gdi32 user32 winhttp shell32)
  endif()
  if(APPLE)
    target_link_libraries(ASB PRIVATE "-framework Cocoa" "-framework CoreText" "-framework QuartzCore" "-framework Foundation")
    smtg_target_set_bundle(ASB BUNDLE_IDENTIFIER lol.maai.asb COMPANY_NAME "maai")
  endif()
  # ---- Audio Unit (macOS, Xcode generator + Apple AudioUnitSDK in third_party/AudioUnitSDK)
  # The SDK's AU wrapper hosts a copy of ASB.vst3 inside ASB.component: same engine, same window.
  if(APPLE AND XCODE)
    set(public_sdk_SOURCE_DIR "${ASB_VST3_SDK_PATH}/public.sdk")
    include(SMTG_AddVST3AuV2)
    if(SMTG_ENABLE_AUV2_BUILDS)
      math(EXPR ASB_AU_VERSION "(${PROJECT_VERSION_MAJOR} << 16) | (${PROJECT_VERSION_MINOR} << 8) | ${PROJECT_VERSION_PATCH}")
      configure_file(resource/au-info.plist.in ${CMAKE_BINARY_DIR}/au-info.plist @ONLY)
      smtg_target_add_auv2(ASB_AU
        BUNDLE_NAME ASB
        BUNDLE_IDENTIFIER lol.maai.asb.audiounit
        INFO_PLIST_TEMPLATE ${CMAKE_BINARY_DIR}/au-info.plist
        VST3_PLUGIN_TARGET ASB)
    else()
      message(WARNING "AudioUnitSDK not found (third_party/AudioUnitSDK): AU target skipped")
    endif()
  endif()

  if(TARGET validator)   # always build it, so it can be run by hand / in CI even with ASB_RUN_VALIDATOR=OFF
    add_dependencies(ASB validator)
  endif()

  if(ASB_BUILD_TESTS)
    add_executable(state_tests tests/state_tests.cpp "${ASB_VST3_SDK_PATH}/public.sdk/source/common/memorystream.cpp")
    target_include_directories(state_tests PRIVATE src)
    target_link_libraries(state_tests PRIVATE sdk asb_engine)
    add_test(NAME state_tests COMMAND state_tests)
  endif()

  if(ASB_BUILD_TESTS AND WIN32)   # opens the plugin window without a DAW (+ optional screenshot)
    add_executable(editor_preview tests/editor_preview.cpp src/vst/Processor.cpp src/vst/Controller.cpp
                   src/ui/EditorCore.cpp src/vst/Editor_win.cpp src/vst/Update_win.cpp)
    target_include_directories(editor_preview PRIVATE src)
    target_link_libraries(editor_preview PRIVATE sdk sdk_hosting asb_engine gdiplus gdi32 user32 winhttp shell32)
  endif()

  if(ASB_BUILD_TESTS)   # shared window logic, headless (recording canvas)
    add_executable(editor_core_tests tests/editor_core_tests.cpp src/ui/EditorCore.cpp src/vst/Controller.cpp src/vst/Editor_none.cpp src/vst/Update_none.cpp)
    target_include_directories(editor_core_tests PRIVATE src)
    target_link_libraries(editor_core_tests PRIVATE sdk asb_engine)
    add_test(NAME editor_core_tests COMMAND editor_core_tests)
  endif()

  if(ASB_BUILD_TESTS AND APPLE)   # opens the plugin window without a DAW (+ PNG screenshot)
    add_executable(editor_preview_mac tests/editor_preview_mac.mm src/vst/Processor.cpp src/vst/Controller.cpp
                   src/ui/EditorCore.cpp src/vst/Editor_mac.mm src/vst/Update_mac.mm)
    target_include_directories(editor_preview_mac PRIVATE src)
    target_link_libraries(editor_preview_mac PRIVATE sdk sdk_hosting asb_engine
                          "-framework Cocoa" "-framework CoreText" "-framework QuartzCore" "-framework Foundation")
  endif()

  if(ASB_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")  # bench uses /proc + clock_gettime (Linux only for now)
    add_executable(processor_bench tests/processor_bench.cpp src/vst/Processor.cpp)
    target_include_directories(processor_bench PRIVATE src)
    target_link_libraries(processor_bench PRIVATE sdk sdk_hosting asb_engine)
  endif()
endif()
