cmake_minimum_required(VERSION 3.20)
project(RTX_VSR_Tool VERSION 1.0.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CUDA architectures — support Maxwell(50), Pascal(61), Volta(70), Turing(75), Ampere(80,86), Ada(89), Blackwell(120)
set(CMAKE_CUDA_ARCHITECTURES "75;86;89")

# CUDA Toolkit (modern replacement for deprecated find_package(CUDA))
find_package(CUDAToolkit REQUIRED)

# ffmpeg (project-local path)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FFmpeg REQUIRED)

# RTX Video SDK paths
set(RTX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RTX_Video_SDK_v1.1.0")
set(RTX_SDK_INCLUDE_DIR "${RTX_SDK_DIR}/include")

# DLSS SDK (newer NGX Core, compatible with modern drivers)
set(DLSS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../DLSS-310.6.0")
set(NGX_CORE_LIB "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.cu)

add_executable(RTX_VSR_Tool WIN32 ${SOURCES})

target_include_directories(RTX_VSR_Tool PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${RTX_SDK_INCLUDE_DIR}
)

# Tell MSVC to treat source files as UTF-8 and emit UTF-8 (fixes Chinese chars)
target_compile_options(RTX_VSR_Tool PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
)

# Dear ImGui
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui.cmake)

target_link_libraries(RTX_VSR_Tool PRIVATE
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    CUDA::cudart
    CUDA::cuda_driver
    "${NGX_CORE_LIB}"
    imgui
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
    shell32
    comctl32
    advapi32
)

# Copy SDK DLLs to output dir
add_custom_command(TARGET RTX_VSR_Tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RTX_SDK_DIR}/bin/Windows/x64/rel/nvngx_vsr.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/nvngx_vsr.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RTX_SDK_DIR}/bin/Windows/x64/rel/nvngx_truehdr.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/nvngx_truehdr.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_DIR}/bin/avcodec-62.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/avcodec-62.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_DIR}/bin/avformat-62.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/avformat-62.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_DIR}/bin/avutil-60.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/avutil-60.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_DIR}/bin/swscale-9.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/swscale-9.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_DIR}/bin/swresample-6.dll"
        "$<TARGET_FILE_DIR:RTX_VSR_Tool>/swresample-6.dll"
)
