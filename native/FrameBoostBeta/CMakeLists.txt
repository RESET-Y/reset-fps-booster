cmake_minimum_required(VERSION 3.20)
project(FrameBoostBeta LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FRAMEBOOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../FrameBoost/src")

find_program(FXC_EXE fxc.exe
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
)
if(NOT FXC_EXE)
    message(FATAL_ERROR "fxc.exe (HLSL shader compiler) not found - is the Windows SDK installed?")
endif()

# Same three shaders as native/FrameBoost - reused as-is (single source of
# truth), just recompiled into this project's own generated/ folder so this
# target stays independently buildable.
set(MOTION_SHADER_SRC "${FRAMEBOOST_SRC}/shaders/motion_estimation.hlsl")
set(MOTION_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_estimation_cs.h")
set(SMOOTH_SHADER_SRC "${FRAMEBOOST_SRC}/shaders/motion_smooth.hlsl")
set(SMOOTH_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/motion_smooth_cs.h")
set(INTERP_SHADER_SRC "${FRAMEBOOST_SRC}/shaders/frame_interpolation.hlsl")
set(INTERP_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/frame_interpolation_cs.h")

add_custom_command(OUTPUT "${MOTION_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${MOTION_SHADER_HEADER}" /Vn g_MotionEstimationCS "${MOTION_SHADER_SRC}"
    DEPENDS "${MOTION_SHADER_SRC}" COMMENT "Compiling motion_estimation.hlsl")
add_custom_command(OUTPUT "${SMOOTH_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${SMOOTH_SHADER_HEADER}" /Vn g_MotionSmoothCS "${SMOOTH_SHADER_SRC}"
    DEPENDS "${SMOOTH_SHADER_SRC}" COMMENT "Compiling motion_smooth.hlsl")
add_custom_command(OUTPUT "${INTERP_SHADER_HEADER}"
    COMMAND "${FXC_EXE}" /nologo /T cs_5_0 /E CSMain /Fh "${INTERP_SHADER_HEADER}" /Vn g_FrameInterpolationCS "${INTERP_SHADER_SRC}"
    DEPENDS "${INTERP_SHADER_SRC}" COMMENT "Compiling frame_interpolation.hlsl")
add_custom_target(CompileBetaShaders DEPENDS "${MOTION_SHADER_HEADER}" "${SMOOTH_SHADER_HEADER}" "${INTERP_SHADER_HEADER}")

add_executable(FrameBoostBeta WIN32
    src/main.cpp
    src/logger.cpp
    src/capture_engine.cpp
    src/beta_presenter.cpp
    src/duplicate_detector.cpp
    "${FRAMEBOOST_SRC}/motion_estimation.cpp"
    "${FRAMEBOOST_SRC}/interpolation.cpp"
    "${FRAMEBOOST_SRC}/logger.cpp"
)

target_include_directories(FrameBoostBeta PRIVATE
    src
    "${FRAMEBOOST_SRC}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/cppwinrt"
)

target_compile_definitions(FrameBoostBeta PRIVATE UNICODE _UNICODE)
target_link_libraries(FrameBoostBeta PRIVATE d3d11.lib dxgi.lib windowsapp.lib)
add_dependencies(FrameBoostBeta CompileBetaShaders)

set_target_properties(FrameBoostBeta PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
