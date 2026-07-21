cmake_minimum_required(VERSION 3.16)

project(use_nvjpeg LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

add_executable(nvjpeg_yuv422_bench src/yuv422_nvjpeg_bench.cpp)
target_compile_features(nvjpeg_yuv422_bench PRIVATE cxx_std_17)
target_compile_options(nvjpeg_yuv422_bench PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nvjpeg_yuv422_bench PRIVATE PkgConfig::GSTREAMER Threads::Threads)
