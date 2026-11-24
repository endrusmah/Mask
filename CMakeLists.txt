cmake_minimum_required(VERSION 3.24)
project(MaskDeterministicEngine VERSION 2.4.0 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "MaskDeterministicEngine supports only Windows x64 with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force x64 architecture check
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MaskDeterministicEngine requires a 64-bit (x86_64) target architecture.")
endif()

find_program(DOTNET_EXECUTABLE dotnet REQUIRED)

add_executable(MaskEngine
    src/main.cpp
    src/app_window.cpp
    src/app_commands.cpp
    src/app_tray.cpp
    src/app_settings_io.cpp
    src/app_engine_threads.cpp
    src/app_selftest.cpp
    src/app_model.cpp
    src/app_settings.cpp
    src/app_view.cpp
    src/app_view_editors.cpp
    src/profile_json.cpp
    src/ui_anim.cpp
    src/ui_gpu_renderer.cpp
    src/timing_engine.cpp
    src/hidmaestro_bridge.cpp
    src/input_pipeline.cpp
    src/macro_runtime.cpp
    src/macro_runtime_avx2.cpp
    src/pixel_trigger_engine.cpp
    src/complex_sequence_engine.cpp
    src/remap_engine.cpp
    src/watchdog.cpp
    resources/mask.rc
)

target_include_directories(MaskEngine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_definitions(MaskEngine PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(MaskEngine PRIVATE
    /utf-8       # source and execution character sets are UTF-8, whatever the PC's code page
    /W4
    /wd4324
    /WX-
    /permissive-
    /Zc:__cplusplus
    /EHsc
    $<$<CONFIG:Release>:/O2 /Oi /Ot /GL /fp:fast>
    $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot /Zi /fp:fast>
)

# Isolate AVX2 codegen strictly to the CPUID-gated runtime kernel TU.
set_source_files_properties(src/macro_runtime_avx2.cpp PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2"
)

target_link_options(MaskEngine PRIVATE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    # GUI app: no console window next to the main window. main() stays the entry point, and
    # `MaskEngine.exe --self-test` still prints to the terminal it was started from.
    /SUBSYSTEM:WINDOWS
    /ENTRY:mainCRTStartup
)

target_link_libraries(MaskEngine PRIVATE
    kernel32
    user32
    shell32
    comdlg32
    advapi32
    ole32
    psapi
    wtsapi32
    winmm
    avrt
    d3d11
    dxgi
    d2d1
    dwrite
    d3dcompiler
    gdi32
    dwmapi
    gameinput
)

# Dependency-tracked incremental build for Mask.HIDMaestroBridge.dll and HIDMaestro.Core.dll staging.
set(MASK_BRIDGE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/bridge-$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${MASK_BRIDGE_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E env --unset=LIB
            "${DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/Mask.HIDMaestroBridge.csproj"
            -c Release
            -o "$<TARGET_FILE_DIR:MaskEngine>"
            --nologo
            -v quiet
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/HIDMaestro.Core.dll"
            "$<TARGET_FILE_DIR:MaskEngine>/HIDMaestro.Core.dll"
    COMMAND ${CMAKE_COMMAND} -E touch "${MASK_BRIDGE_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/Mask.HIDMaestroBridge.csproj"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/HIDMaestroBridge.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/HIDMaestro.Core.dll"
    COMMENT "Building Mask.HIDMaestroBridge.dll (only when bridge files change)..."
    VERBATIM
)
add_custom_target(MaskBridge ALL DEPENDS "${MASK_BRIDGE_STAMP}")
add_dependencies(MaskEngine MaskBridge)
