cmake_minimum_required(VERSION 3.20)
project(FrameBoostProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Precompile the motion-estimation compute shader offline via fxc into a C
# header (byte array). This avoids depending on d3dcompiler_47.dll being
# present at runtime on the target machine, and avoids any runtime shader
# compile overhead - the shader is ready-to-use bytecode from the start.
find_program(FXC_EXE fxc.exe
    PATHS "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}x64"
    PATH_SUFFIXES x64
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
)
if(NOT FXC_EXE)
    message(FATAL_ERROR "fxc.exe (HLSL shader compiler) not found - is the Windows SDK installed?")
endif()

set(MOTION_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/motion_estimation.hlsl")
set(MOTION_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_estimation_cs.h")
set(COARSE_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/motion_estimation_coarse.hlsl")
set(COARSE_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_estimation_coarse_cs.h")
set(COARSEST_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/motion_estimation_coarsest.hlsl")
set(COARSEST_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_estimation_coarsest_cs.h")
set(SMOOTH_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/motion_smooth.hlsl")
set(SMOOTH_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_smooth_cs.h")
set(INTERP_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/frame_interpolation.hlsl")
set(INTERP_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/frame_interpolation_cs.h")

add_custom_command(
    OUTPUT "${MOTION_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${MOTION_SHADER_HEADER}" /Vn g_MotionEstimationCS "${MOTION_SHADER_SRC}"
    DEPENDS "${MOTION_SHADER_SRC}"
    COMMENT "Compiling motion_estimation.hlsl -> motion_estimation_cs.h"
)
add_custom_command(
    OUTPUT "${COARSE_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${COARSE_SHADER_HEADER}" /Vn g_MotionEstimationCoarseCS "${COARSE_SHADER_SRC}"
    DEPENDS "${COARSE_SHADER_SRC}"
    COMMENT "Compiling motion_estimation_coarse.hlsl -> motion_estimation_coarse_cs.h"
)
add_custom_command(
    OUTPUT "${COARSEST_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${COARSEST_SHADER_HEADER}" /Vn g_MotionEstimationCoarsestCS "${COARSEST_SHADER_SRC}"
    DEPENDS "${COARSEST_SHADER_SRC}"
    COMMENT "Compiling motion_estimation_coarsest.hlsl -> motion_estimation_coarsest_cs.h"
)
add_custom_command(
    OUTPUT "${SMOOTH_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${SMOOTH_SHADER_HEADER}" /Vn g_MotionSmoothCS "${SMOOTH_SHADER_SRC}"
    DEPENDS "${SMOOTH_SHADER_SRC}"
    COMMENT "Compiling motion_smooth.hlsl -> motion_smooth_cs.h"
)
add_custom_command(
    OUTPUT "${INTERP_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${INTERP_SHADER_HEADER}" /Vn g_FrameInterpolationCS "${INTERP_SHADER_SRC}"
    DEPENDS "${INTERP_SHADER_SRC}"
    COMMENT "Compiling frame_interpolation.hlsl -> frame_interpolation_cs.h"
)
add_custom_target(CompileShaders DEPENDS "${MOTION_SHADER_HEADER}" "${COARSE_SHADER_HEADER}" "${COARSEST_SHADER_HEADER}" "${SMOOTH_SHADER_HEADER}" "${INTERP_SHADER_HEADER}")

# Milestone 1 target: a d3d11.dll proxy that sits next to Watch_Dogs.exe.
# It forwards every real export to the system d3d11.dll so the game keeps
# working normally, and additionally hooks IDXGISwapChain::Present so we can
# observe/capture real frames. No injection into other processes, no kernel
# component - this is the same technique overlay tools like ReShade use.
add_library(d3d11 SHARED
    src/dllmain.cpp
    src/present_hook.cpp
    src/logger.cpp
    src/bmp_writer.cpp
    src/motion_estimation.cpp
    src/interpolation.cpp
)

target_include_directories(d3d11 PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(d3d11 PRIVATE d3d11.lib dxgi.lib)
add_dependencies(d3d11 CompileShaders)

# Keep the proxy's own export table minimal and exact - only the real
# d3d11.dll exports, nothing else, so the game's loader is satisfied.
target_link_options(d3d11 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/d3d11_proxy.def")

# ARCHIVE_OUTPUT_NAME (the .lib import library CMake also generates for a
# DLL target) is renamed away from "d3d11" - otherwise it collides with the
# real Windows SDK d3d11.lib we link against, since both would sit in the
# same build directory and the linker's own output would shadow its input.
set_target_properties(d3d11 PROPERTIES
    OUTPUT_NAME "d3d11"
    ARCHIVE_OUTPUT_NAME "d3d11_proxy_implib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Path B, confirmed necessary by real testing against Watch Dogs (2014):
# the game creates its swapchain via IDXGIFactory::CreateSwapChain rather
# than D3D11CreateDeviceAndSwapChain, so a dxgi.dll proxy is what actually
# catches it - same technique this game's existing ReShade install uses.
add_library(dxgi SHARED
    src/dxgi_dllmain.cpp
    src/factory_hook.cpp
    src/present_hook.cpp
    src/logger.cpp
    src/bmp_writer.cpp
    src/motion_estimation.cpp
    src/interpolation.cpp
)

target_include_directories(dxgi PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(dxgi PRIVATE dxgi.lib d3d11.lib)
add_dependencies(dxgi CompileShaders)
target_link_options(dxgi PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/dxgi_proxy.def")

set_target_properties(dxgi PROPERTIES
    OUTPUT_NAME "dxgi"
    ARCHIVE_OUTPUT_NAME "dxgi_proxy_implib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
