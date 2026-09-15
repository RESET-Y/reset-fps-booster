cmake_minimum_required(VERSION 3.20)
project(FrameBoostV2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The proven GPU core, lifted out of the old engine and now a tree of its own.
# See native/FrameBoostLegacy/README.md for why it was copied rather than
# moved: the folder it came from also builds d3d11/dxgi proxy DLLs, which this
# project does not ship and must not depend on.
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FrameBoostCore")

find_program(FXC_EXE fxc.exe
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
)
if(NOT FXC_EXE)
    message(FATAL_ERROR "fxc.exe (HLSL shader compiler) not found - is the Windows SDK installed?")
endif()

# Each shader becomes a byte-array header. Nothing here is checked in: the
# headers are reproducible from the .hlsl sources, so build/ stays ignored.
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

function(compile_shader SRC_NAME OUT_NAME SYMBOL)
    add_custom_command(
        OUTPUT "${GEN_DIR}/${OUT_NAME}"
        COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain
                /Fh "${GEN_DIR}/${OUT_NAME}" /Vn ${SYMBOL}
                "${CORE_DIR}/shaders/${SRC_NAME}"
        DEPENDS "${CORE_DIR}/shaders/${SRC_NAME}"
        COMMENT "Compiling ${SRC_NAME}")
endfunction()

compile_shader(motion_estimation.hlsl           motion_estimation_cs.h           g_MotionEstimationCS)
compile_shader(motion_estimation_coarse.hlsl    motion_estimation_coarse_cs.h    g_MotionEstimationCoarseCS)
compile_shader(motion_estimation_coarsest.hlsl  motion_estimation_coarsest_cs.h  g_MotionEstimationCoarsestCS)
compile_shader(motion_smooth.hlsl               motion_smooth_cs.h               g_MotionSmoothCS)
compile_shader(frame_interpolation.hlsl         frame_interpolation_cs.h         g_FrameInterpolationCS)
compile_shader(upscale.hlsl                     upscale_cs.h                     g_UpscaleCS)
compile_shader(thumbnail.hlsl                   thumbnail_cs.h                   g_ThumbnailCS)

add_custom_target(CompileV2Shaders DEPENDS
    "${GEN_DIR}/motion_estimation_cs.h"
    "${GEN_DIR}/motion_estimation_coarse_cs.h"
    "${GEN_DIR}/motion_estimation_coarsest_cs.h"
    "${GEN_DIR}/motion_smooth_cs.h"
    "${GEN_DIR}/frame_interpolation_cs.h"
    "${GEN_DIR}/upscale_cs.h"
    "${GEN_DIR}/thumbnail_cs.h"
)

# FrameBoostBeta, deliberately. The C# side starts
# <AppDir>/FrameBoostBeta/FrameBoostBeta.exe with the arguments
# "window slotwait" and reads the log it writes. Keeping the name means the
# app needs no change and the machine needs no .NET SDK, which it has not got.
add_executable(FrameBoostBeta WIN32
    src/main.cpp
    src/capture.cpp
    src/presenter.cpp
    src/telemetry.cpp
    src/synthetic.cpp
    src/logger.cpp
    "${CORE_DIR}/src/motion_estimation.cpp"
    "${CORE_DIR}/src/interpolation.cpp"
    "${CORE_DIR}/src/logger.cpp"
)

target_include_directories(FrameBoostBeta PRIVATE
    src
    "${CORE_DIR}/src"
    "${GEN_DIR}"
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/cppwinrt"
)

target_compile_definitions(FrameBoostBeta PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(FrameBoostBeta PRIVATE d3d11.lib dxgi.lib dcomp.lib windowsapp.lib winmm.lib)
add_dependencies(FrameBoostBeta CompileV2Shaders)

set_target_properties(FrameBoostBeta PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
