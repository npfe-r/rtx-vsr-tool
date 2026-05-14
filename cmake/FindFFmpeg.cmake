# FindFFmpeg.cmake — locates ffmpeg installed at project-level path
# Provides: FFmpeg::libavcodec, FFmpeg::libavformat, FFmpeg::libavutil, FFmpeg::libswscale

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg-8.1.1-full_build-shared"
    CACHE PATH "Path to ffmpeg installation directory")

foreach(component avcodec avformat avutil swscale swresample)
    find_path(FFmpeg_${component}_INCLUDE_DIR
        NAMES lib${component}/${component}.h
        PATHS "${FFMPEG_DIR}/include"
        NO_DEFAULT_PATH
    )
    find_library(FFmpeg_${component}_LIBRARY
        NAMES ${component} ${component}.lib
        PATHS "${FFMPEG_DIR}/lib"
        NO_DEFAULT_PATH
    )
    mark_as_advanced(FFmpeg_${component}_INCLUDE_DIR FFmpeg_${component}_LIBRARY)

    if(FFmpeg_${component}_INCLUDE_DIR AND FFmpeg_${component}_LIBRARY)
        set(FFmpeg_${component}_FOUND TRUE)
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_avcodec_LIBRARY FFmpeg_avcodec_INCLUDE_DIR
                  FFmpeg_avformat_LIBRARY FFmpeg_avformat_INCLUDE_DIR
                  FFmpeg_avutil_LIBRARY FFmpeg_avutil_INCLUDE_DIR
                  FFmpeg_swscale_LIBRARY FFmpeg_swscale_INCLUDE_DIR
)

if(FFmpeg_FOUND)
    foreach(component avcodec avformat avutil swscale swresample)
        if(NOT TARGET FFmpeg::${component})
            add_library(FFmpeg::${component} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${component} PROPERTIES
                IMPORTED_LOCATION "${FFmpeg_${component}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${component}_INCLUDE_DIR}"
            )
        endif()
    endforeach()
endif()
