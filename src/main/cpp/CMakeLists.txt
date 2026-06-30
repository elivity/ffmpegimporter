cmake_minimum_required(VERSION 3.22.1)

project(ffmpegimporter LANGUAGES C CXX)

# Gradle should pass these:
# -DFFMPEG_JNI_LIBS_DIR=/.../src/main/jniLibs
# -DFFMPEG_INCLUDE_DIR=/.../src/main/cpp/ffmpeg/include
#
# Fallbacks are useful when opening CMake manually.
if (NOT DEFINED FFMPEG_JNI_LIBS_DIR)
    set(FFMPEG_JNI_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs")
endif ()

if (NOT DEFINED FFMPEG_INCLUDE_DIR)
    set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/include")
endif ()

set(FFMPEG_LIBS_DIR "${FFMPEG_JNI_LIBS_DIR}/${ANDROID_ABI}")
set(FFMPEG_HEADERS_DIR "${FFMPEG_INCLUDE_DIR}/${ANDROID_ABI}")

message(STATUS "ANDROID_ABI=${ANDROID_ABI}")
message(STATUS "FFMPEG_LIBS_DIR=${FFMPEG_LIBS_DIR}")
message(STATUS "FFMPEG_HEADERS_DIR=${FFMPEG_HEADERS_DIR}")

set(ffmpeg_libs_names
        avutil
        avformat
        avcodec
        swscale
        swresample
)

foreach (ffmpeg_lib_name ${ffmpeg_libs_names})
    add_library(${ffmpeg_lib_name} SHARED IMPORTED)

    set_target_properties(${ffmpeg_lib_name}
            PROPERTIES
            IMPORTED_LOCATION
            "${FFMPEG_LIBS_DIR}/lib${ffmpeg_lib_name}.so"
    )
endforeach ()

add_library(ffmpegimporter
        SHARED
        jni_bridge.cpp
        FFMpegExtractor.cpp
)

target_include_directories(ffmpegimporter
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${FFMPEG_HEADERS_DIR}"
)

target_link_libraries(ffmpegimporter
        PRIVATE
        ${ffmpeg_libs_names}
        android
        atomic
        log
)

target_compile_features(ffmpegimporter PRIVATE cxx_std_17)

target_compile_options(ffmpegimporter
        PRIVATE
        -Ofast
        -Werror
)

target_link_options(ffmpegimporter
        PRIVATE
        -Wl,-z,max-page-size=16384
)
