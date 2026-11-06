# LegacyAVR - CMake build for the native engine + tests (alternative to LegacyAVR.sln).
# The WPF UI is built from LegacyAVR.sln or `dotnet build ui\LegacyAVR.UI`.
#
#   cmake --preset vs2026-x64
#   cmake --build --preset release
#   ctest --preset release
cmake_minimum_required(VERSION 3.31)
project(LegacyAVR VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "LegacyAVR targets Windows 11 with MSVC (Visual Studio 2026) only.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# FFmpeg via vcpkg manifest mode (toolchain set in CMakePresets.json)
find_package(FFMPEG REQUIRED)

add_library(legacyavr_options INTERFACE)
target_compile_definitions(legacyavr_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(legacyavr_options INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP /sdl
    /external:anglebrackets /external:W0 /wd4324
    $<$<CONFIG:Release>:/O2 /Oi /Gy /arch:AVX2 /fp:fast>)

# ---- core library -----------------------------------------------------------------
add_library(LegacyAVR.Core STATIC
    engine/core/BassManager.cpp
    engine/core/ChannelEq.cpp
    engine/core/ChannelMatrix.cpp
    engine/core/Dynamics.cpp
    engine/core/Encoder.cpp
    engine/core/FrequencyRange.cpp
    engine/core/Iec61937.cpp
    engine/core/OutputPolicy.cpp
    engine/core/ProcessingCore.cpp
    engine/core/Resampler.cpp
    engine/core/TestSignals.cpp
    engine/core/Upmixer.cpp
    engine/ipc/ControlProtocol.cpp
    engine/win/ConfigStore.cpp
    engine/win/EngineHost.cpp
    engine/win/Logger.cpp
    engine/win/PipeServer.cpp
    engine/win/SpeechClips.cpp
    engine/win/TelemetryWriter.cpp
    engine/win/WasapiCapture.cpp
    engine/win/WasapiDevices.cpp
    engine/win/WasapiRenderer.cpp
    engine/win/WinUtil.cpp)
target_include_directories(LegacyAVR.Core PUBLIC engine ${FFMPEG_INCLUDE_DIRS})
target_link_directories(LegacyAVR.Core PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(LegacyAVR.Core
    PUBLIC ${FFMPEG_LIBRARIES} avrt ole32 propsys shlwapi shell32 advapi32
    PRIVATE legacyavr_options)

# ---- executables ---------------------------------------------------------------------
add_executable(AVREngine engine/app/AVREngine/main.cpp engine/app/AVREngine/AVREngine.rc)
target_link_libraries(AVREngine PRIVATE LegacyAVR.Core legacyavr_options)

add_executable(AVREngineService engine/app/AVREngineService/ServiceMain.cpp)
target_link_libraries(AVREngineService PRIVATE LegacyAVR.Core legacyavr_options)

# ---- tests ---------------------------------------------------------------------------
enable_testing()
add_executable(LegacyAVR.Tests tests/TestMain.cpp tests/test_core.cpp tests/test_codec.cpp tests/bench.cpp)
target_link_libraries(LegacyAVR.Tests PRIVATE LegacyAVR.Core legacyavr_options)
add_test(NAME unit COMMAND LegacyAVR.Tests)
add_test(NAME benchmark COMMAND LegacyAVR.Tests --bench)
set_tests_properties(benchmark PROPERTIES LABELS perf)
# FFmpeg DLLs are copied next to each executable by the vcpkg toolchain
# (VCPKG_APPLOCAL_DEPS is ON by default).
