# FindTensorRT.cmake
# 查找 TensorRT 安装路径。优先级:
#   1. TensorRT_ROOT 环境变量
#   2. TENSORRT_ROOT 环境变量
#   3. 常见默认安装路径

if(DEFINED ENV{TensorRT_ROOT})
    set(TENSORRT_ROOT "$ENV{TensorRT_ROOT}")
elseif(DEFINED ENV{TENSORRT_ROOT})
    set(TENSORRT_ROOT "$ENV{TENSORRT_ROOT}")
else()
    # 尝试多个常见安装路径
    set(_trt_candidates
        "C:/Program Files/NVIDIA GPU Computing Toolkit/TensorRT"
        "C:/Program Files/TensorRT-10.16.1.11"
        "C:/Program Files/TensorRT-10.15.1.10"
        "C:/Program Files/TensorRT-10.14.1.9"
        "C:/Program Files/TensorRT-10.13.1.8"
        "C:/Program Files/TensorRT-10.12.1.7"
        "C:/Program Files/TensorRT-10.11.1.6"
        "C:/Program Files/TensorRT-10.10.1.5"
        "C:/Program Files/TensorRT-10.9.1.4"
        "C:/Program Files/TensorRT-10.8.1.3"
        "C:/Program Files/TensorRT-10.7.1.2"
        "C:/Program Files/TensorRT-10.6.1.1"
        "C:/Program Files/TensorRT-10.5.1.0"
    )
    foreach(_path ${_trt_candidates})
        if(EXISTS "${_path}/include/NvInfer.h")
            set(TENSORRT_ROOT "${_path}")
            break()
        endif()
    endforeach()
    if(NOT TENSORRT_ROOT)
        set(TENSORRT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/TensorRT")
    endif()
endif()

find_path(TensorRT_INCLUDE_DIRS
    NAMES NvInfer.h
    PATHS "${TENSORRT_ROOT}/include"
    NO_DEFAULT_PATH
)

# TensorRT 10.x 库文件在 lib/，旧版在 lib/x64/；库名带 _10 后缀
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_lib_subdirs "lib" "lib/x64")
else()
    set(_lib_subdirs "lib" "lib/x86")
endif()

macro(_find_trt_lib _var _name)
    foreach(_sub ${_lib_subdirs})
        # 使用 find_library 而非文件检查，以便利用 CMake 缓存
        find_library(${_var}
            NAMES ${_name}_10 ${_name}_10.lib ${_name} ${_name}.lib
            PATHS "${TENSORRT_ROOT}/${_sub}"
            NO_DEFAULT_PATH
        )
        if(${_var})
            break()
        endif()
    endforeach()
endmacro()

_find_trt_lib(TensorRT_LIBRARY                nvinfer)
_find_trt_lib(TensorRT_NVONNXPARSER_LIBRARY   nvonnxparser)
_find_trt_lib(TensorRT_NVINFERPLUGIN_LIBRARY  nvinfer_plugin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TensorRT
    REQUIRED_VARS TensorRT_INCLUDE_DIRS
                  TensorRT_LIBRARY
                  TensorRT_NVONNXPARSER_LIBRARY
)

if(TensorRT_FOUND)
    set(TensorRT_LIBRARIES
        "${TensorRT_LIBRARY}"
        "${TensorRT_NVONNXPARSER_LIBRARY}"
        "${TensorRT_NVINFERPLUGIN_LIBRARY}"
    )
    # 提取版本号（兼容社区版和 Enterprise 版）
    set(NV_TENSORRT_MAJOR "")
    set(NV_TENSORRT_MINOR "")
    set(NV_TENSORRT_PATCH "")
    file(STRINGS "${TensorRT_INCLUDE_DIRS}/NvInferVersion.h" _trt_ver_maj
         REGEX "#define NV_TENSORRT_MAJOR|#define TRT_MAJOR_ENTERPRISE")
    file(STRINGS "${TensorRT_INCLUDE_DIRS}/NvInferVersion.h" _trt_ver_min
         REGEX "#define NV_TENSORRT_MINOR|#define TRT_MINOR_ENTERPRISE")
    file(STRINGS "${TensorRT_INCLUDE_DIRS}/NvInferVersion.h" _trt_ver_pat
         REGEX "#define NV_TENSORRT_PATCH|#define TRT_PATCH_ENTERPRISE")
    string(REGEX REPLACE ".*NV_TENSORRT_MAJOR[ ]+([0-9]+).*" "\\1" _maj "${_trt_ver_maj}")
    if(NOT _maj OR _maj STREQUAL _trt_ver_maj)
        string(REGEX REPLACE ".*TRT_MAJOR_ENTERPRISE[ ]+([0-9]+).*" "\\1" _maj "${_trt_ver_maj}")
    endif()
    string(REGEX REPLACE ".*NV_TENSORRT_MINOR[ ]+([0-9]+).*" "\\1" _min "${_trt_ver_min}")
    if(NOT _min OR _min STREQUAL _trt_ver_min)
        string(REGEX REPLACE ".*TRT_MINOR_ENTERPRISE[ ]+([0-9]+).*" "\\1" _min "${_trt_ver_min}")
    endif()
    string(REGEX REPLACE ".*NV_TENSORRT_PATCH[ ]+([0-9]+).*" "\\1" _pat "${_trt_ver_pat}")
    if(NOT _pat OR _pat STREQUAL _trt_ver_pat)
        string(REGEX REPLACE ".*TRT_PATCH_ENTERPRISE[ ]+([0-9]+).*" "\\1" _pat "${_trt_ver_pat}")
    endif()
    set(TensorRT_VERSION "${_maj}.${_min}.${_pat}")
    mark_as_advanced(TensorRT_INCLUDE_DIRS TensorRT_LIBRARY
                     TensorRT_NVONNXPARSER_LIBRARY)
endif()
