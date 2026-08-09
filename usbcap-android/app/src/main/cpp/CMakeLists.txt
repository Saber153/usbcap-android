cmake_minimum_required(VERSION 3.22.1)
project("usbcap_native")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Android NDK库
find_library(log-lib log)
find_library(android-lib android)
find_library(GLESv2-lib GLESv2)
find_library(EGL-lib EGL)

add_library(usbcap_native SHARED
    native_renderer.cpp
)

target_link_libraries(usbcap_native
    ${log-lib}
    ${android-lib}
    ${GLESv2-lib}
    ${EGL-lib}
)
