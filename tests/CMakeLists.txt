cmake_minimum_required(VERSION 3.16)
project(cs2-vr-spectator-tests CXX)

# Deliberately standalone. It does not know about advancedfx, CS2, Windows or OpenXR,
# because the code it tests does not either -- that is the only reason these tests can run
# on a Linux runner in a couple of seconds while the hook itself needs a Windows toolchain
# and half an hour.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_executable(test-mirv-vr-math test-mirv-vr-math.cpp)

if(MSVC)
    target_compile_options(test-mirv-vr-math PRIVATE /W4)
else()
    target_compile_options(test-mirv-vr-math PRIVATE -Wall -Wextra)
endif()

add_test(NAME mirv-vr-math COMMAND test-mirv-vr-math)

# The launcher's decisions - where CS2 is, which runtime, what the command line is, whether
# it may start at all - kept free of Windows for the same reason, and tested the same way.
add_executable(test-launcher-logic test-launcher-logic.cpp)

if(MSVC)
    target_compile_options(test-launcher-logic PRIVATE /W4)
else()
    target_compile_options(test-launcher-logic PRIVATE -Wall -Wextra)
endif()

add_test(NAME launcher-logic COMMAND test-launcher-logic)
