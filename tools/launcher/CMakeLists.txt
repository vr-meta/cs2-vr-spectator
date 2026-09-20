cmake_minimum_required(VERSION 3.16)
project(cs2vr CXX)

# The launcher: finds CS2, checks the machine, starts the game with the hook in it.
#
# Windows and x64 only, because that is what CS2 and the hook are. Everything it DECIDES
# is in LauncherLogic.h and is tested from tests/ on any platform; this target is the part
# that touches the registry, the process list and the game.
#
#   cmake -S tools/launcher -B build/launcher -A x64
#   cmake --build build/launcher --config Release
#
# A release sets the two strings below; a developer build says so in its own banner.

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CS2VR_VERSION "0.0.0-dev" CACHE STRING "Version printed by cs2vr.exe")

# Which CS2 build a release is for is the HOOK's fact - its field offsets were measured on
# one build - so it is read out of the hook's own version header rather than kept here as
# a second copy that would one day disagree with what the hook prints into console.log.
set(_afxvr_version_header "${CMAKE_CURRENT_SOURCE_DIR}/../../src/AfxHookSource2/MirvVrVersion.h")
file(STRINGS "${_afxvr_version_header}" _afxvr_build_line
     REGEX "#define[ \t]+AFXVR_TESTED_CLIENT_VERSION[ \t]+\"[0-9]+\"")
string(REGEX MATCH "\"([0-9]+)\"" _afxvr_unused "${_afxvr_build_line}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "AFXVR_TESTED_CLIENT_VERSION not found in ${_afxvr_version_header}")
endif()
set(CS2VR_CS2_BUILD "${CMAKE_MATCH_1}")
message(STATUS "cs2vr ${CS2VR_VERSION}, for CS2 build ${CS2VR_CS2_BUILD}")

if(NOT WIN32)
    message(FATAL_ERROR "cs2vr is a Windows program. Its logic is tested from tests/ on any platform.")
endif()

add_executable(cs2vr main.cpp LauncherLogic.h)

target_compile_definitions(cs2vr PRIVATE
    CS2VR_VERSION="${CS2VR_VERSION}"
    CS2VR_CS2_BUILD="${CS2VR_CS2_BUILD}"
    UNICODE _UNICODE)

if(MSVC)
    target_compile_options(cs2vr PRIVATE /W4 /utf-8)
    # Static C++ runtime: the exe has to run on a machine with nothing installed.
    set_property(TARGET cs2vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(cs2vr PRIVATE /SUBSYSTEM:CONSOLE)
endif()

target_link_libraries(cs2vr PRIVATE advapi32 shell32)
