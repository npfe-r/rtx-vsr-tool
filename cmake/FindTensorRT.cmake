# FindTensorRT.cmake
# 查找 TensorRT 安装路径。优先级:
#   1. TensorRT_ROOT 环境变量
#   2. TENSORRT_ROOT 环境变量
#   3. 默认安装路径

if(DEFINED ENV{TensorRT_ROOT})
    set(TENSORRT_ROOT "$ENV{TensorRT_ROOT}")
elseif(DEFINED ENV{TENSORRT_ROOT})
    set(TENSORRT_ROOT "$ENV{TENSORRT_ROOT}")
else()
    set(TENSORRT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/TensorRT")
endif()

find_path(TensorRT_INCLUDE_DIRS
    NAMES NvInfer.h
    PATHS "${TENSORRT_ROOT}/include"
    NO_DEFAULT_PATH
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_lib_path_suffix "lib/x64")
else()
    set(_lib_path_suffix "lib/x86")
endif()

macro(_find_trt_lib _var _name)
    find_library(${_var}
        NAMES ${_name} ${_name}.lib
        PATHS "${TENSORRT_ROOT}/${_lib_path_suffix}"
        NO_DEFAULT_PATH
    )
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
    # 提取版本号
    file(STRINGS "${TensorRT_INCLUDE_DIRS}/NvInferVersion.h" _trt_ver
         REGEX "#define NV_TENSORRT_MAJOR|#define NV_TENSORRT_MINOR|#define NV_TENSORRT_PATCH")
    string(REGEX REPLACE ".*NV_TENSORRT_MAJOR ([0-9]+).*" "\\1" _maj "${_trt_ver}")
    string(REGEX REPLACE ".*NV_TENSORRT_MINOR ([0-9]+).*" "\\1" _min "${_trt_ver}")
    string(REGEX REPLACE ".*NV_TENSORRT_PATCH ([0-9]+).*" "\\1" _pat "${_trt_ver}")
    set(TensorRT_VERSION "${_maj}.${_min}.${_pat}")
    mark_as_advanced(TensorRT_INCLUDE_DIRS TensorRT_LIBRARY
                     TensorRT_NVONNXPARSER_LIBRARY)
endif()
