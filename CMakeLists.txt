cmake_minimum_required(VERSION 3.20)

project(cs2_cheat CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

# ---- Source files ----
set(SOURCES
    cheat/main.cpp
    cheat/overlay.cpp
    math/vec2.cpp
    math/vec3.cpp
    math/mat3x4.cpp
    math/mat4x4.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx11.cpp
)

add_executable(cs2_cheat ${SOURCES})

target_include_directories(cs2_cheat PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/cheat
    ${CMAKE_SOURCE_DIR}/third_party/imgui
)

target_compile_options(cs2_cheat PRIVATE
    -msse4.1
    -mavx2
    -mno-avx512f
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g3>
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-ignored-attributes
)

# Manifest UAC requireAdministrator
if(EXISTS ${CMAKE_SOURCE_DIR}/cheat/manifest.res)
    target_sources(cs2_cheat PRIVATE ${CMAKE_SOURCE_DIR}/cheat/manifest.res)
endif()

set_target_properties(cs2_cheat PROPERTIES
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Link — selalu include d3d11/dxgi/dcomp/dwmapi/gdi32/shell32
target_link_libraries(cs2_cheat PRIVATE
    kernel32 user32 ntdll shell32 d3d11 d3dcompiler_47 dxgi dcomp dwmapi gdi32 winmm
)

message(STATUS "Compiler  : ${CMAKE_CXX_COMPILER}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
