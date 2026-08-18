cmake_minimum_required(VERSION 3.20)
project(BetterCastGlassSpike LANGUAGES CXX)

# Spike. Builds a BetterCast device page against liquidDX11's glass kit, to
# judge the material with real content and measure what it costs on the
# integrated GPU that also feeds DXGI capture while streaming.
#
# Compiles liquidDX11's sources directly rather than linking it, because that
# project builds an executable around its own main.cpp — this substitutes ours.
# LIQUID_DIR must point at a checkout of StephenLovino/liquidDX11.

if(NOT WIN32)
    message(FATAL_ERROR "The glass spike is Windows-only: Win32, DXGI, D3D11, HLSL.")
endif()

if(NOT DEFINED LIQUID_DIR)
    message(FATAL_ERROR "Set -DLIQUID_DIR=<path to a liquidDX11 checkout>")
endif()

set(APP_DIR "${LIQUID_DIR}/examples/example_win32_directx11")

add_executable(BetterCastGlassSpike WIN32
    main.cpp
    "${LIQUID_DIR}/imgui.cpp"
    "${LIQUID_DIR}/imgui_draw.cpp"
    "${LIQUID_DIR}/imgui_tables.cpp"
    "${LIQUID_DIR}/imgui_widgets.cpp"
    "${LIQUID_DIR}/misc/freetype/imgui_freetype.cpp"
    "${LIQUID_DIR}/backends/imgui_impl_dx11.cpp"
    "${LIQUID_DIR}/backends/imgui_impl_win32.cpp"
    "${APP_DIR}/src/glass/backdrop.cpp"
    "${APP_DIR}/src/glass/glass.cpp"
)

target_compile_features(BetterCastGlassSpike PRIVATE cxx_std_17)

target_include_directories(BetterCastGlassSpike PRIVATE
    "${LIQUID_DIR}"
    "${LIQUID_DIR}/backends"
    "${LIQUID_DIR}/thirdparty/freetype/include"
    "${APP_DIR}/src"
)

target_compile_definitions(BetterCastGlassSpike PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    # /utf-8 matters: glass.cpp and the widget .inc files carry non-ASCII.
    target_compile_options(BetterCastGlassSpike PRIVATE /utf-8)
endif()

target_link_libraries(BetterCastGlassSpike PRIVATE
    d3d11 d3dcompiler dxgi dwmapi windowscodecs
    ole32 winmm user32 gdi32 shell32 comdlg32 advapi32
    "${LIQUID_DIR}/thirdparty/freetype/win64/freetype.lib"
)
