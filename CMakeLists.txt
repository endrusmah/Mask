cmake_minimum_required(VERSION 3.24)
project(MaskDeterministicEngine VERSION 2.3.0 LANGUAGES CXX)
enable_language(RC)

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

if(MSVC)
    target_compile_options(MaskEngine PRIVATE
        /utf-8       # source and execution character sets are UTF-8, whatever the PC's code page
        /W4
        /wd4324
        /WX-
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /arch:AVX2
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GL /fp:fast /GS->
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot /Zi /fp:fast>
    )
    target_link_options(MaskEngine PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
        # GUI app: no console window next to the main window. main() stays the entry point, and
        # `MaskEngine.exe --self-test` still prints to the terminal it was started from.
        /SUBSYSTEM:WINDOWS
        /ENTRY:mainCRTStartup
    )
else()
    target_compile_options(MaskEngine PRIVATE
        -Wall
        -Wextra
        -mavx2
        -mbmi2
        $<$<CONFIG:Release>:-O3 -flto -ffast-math>
    )
    target_link_options(MaskEngine PRIVATE
        $<$<CONFIG:Release>:-flto>
        -mwindows
    )
endif()

target_link_libraries(MaskEngine PRIVATE
    kernel32
    user32
    shell32
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

# Build the Native-Callable C# Bridge (Mask.HIDMaestroBridge.dll) and copy HIDMaestro.Core.dll v2.1.0
# directly alongside MaskEngine.exe on every build.
add_custom_command(TARGET MaskEngine POST_BUILD
    COMMAND "${DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/Mask.HIDMaestroBridge.csproj"
            -c Release
            -o "$<TARGET_FILE_DIR:MaskEngine>"
            --nologo
            -v quiet
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/HIDMaestro.Core.dll"
            "$<TARGET_FILE_DIR:MaskEngine>/HIDMaestro.Core.dll"
    COMMENT "Building Mask.HIDMaestroBridge.dll and staging HIDMaestro.Core.dll v2.1.0..."
)
