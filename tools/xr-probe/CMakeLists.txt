cmake_minimum_required(VERSION 3.16)
project(xr-probe CXX)

# Needs the OpenXR headers and Windows, so it is not part of the tests/ project -- that one
# stays buildable anywhere on purpose. Only the headers are needed here too: the loader is
# opened by path at runtime, exactly as the hook does it.

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED AFXVR_OPENXR_DIR AND DEFINED ENV{AFXVR_OPENXR_DIR})
    set(AFXVR_OPENXR_DIR_DEFAULT "$ENV{AFXVR_OPENXR_DIR}")
else()
    set(AFXVR_OPENXR_DIR_DEFAULT "D:/Dev/cs2-vr-tools/openxr/pkg")
endif()
set(AFXVR_OPENXR_DIR "${AFXVR_OPENXR_DIR_DEFAULT}" CACHE PATH "OpenXR SDK root (headers under include/)")

if(NOT EXISTS "${AFXVR_OPENXR_DIR}/include/openxr/openxr.h")
    message(FATAL_ERROR "OpenXR headers not found at ${AFXVR_OPENXR_DIR}/include/openxr/openxr.h -- pass -DAFXVR_OPENXR_DIR=<sdk root>.")
endif()

add_executable(xr-probe xr-probe.cpp)
target_include_directories(xr-probe PRIVATE "${AFXVR_OPENXR_DIR}/include")
