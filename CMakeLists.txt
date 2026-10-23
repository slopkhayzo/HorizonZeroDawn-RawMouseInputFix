cmake_minimum_required(VERSION 3.24)
project(HZDRawMouse VERSION 1.0.1 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HZDRawMouse must be built for x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the .asi has no runtime dependencies beyond the OS.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(HZDRawMouse SHARED
    src/dllmain.cpp
    src/config.cpp
    src/log.cpp
    src/mem.cpp
    src/hooks_input.cpp
    src/hooks_dxgi.cpp
    src/watch.cpp
    src/raw_camera.cpp
    src/version.rc
)

target_compile_definitions(HZDRawMouse PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(HZDRawMouse PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(HZDRawMouse PRIVATE user32)

# Ultimate ASI Loader picks up *.asi files.
set_target_properties(HZDRawMouse PROPERTIES SUFFIX ".asi")

option(HZDRM_BUILD_TESTS "Build offline discovery test (scan_test.exe)" ON)
if(HZDRM_BUILD_TESTS)
    add_executable(scan_test tests/scan_test.cpp src/config.cpp src/log.cpp src/mem.cpp src/hooks_input.cpp src/watch.cpp src/raw_camera.cpp)
    target_compile_definitions(scan_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(scan_test PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(scan_test PRIVATE user32)

    add_executable(ual_host tests/ual_host.cpp)
endif()

install(TARGETS HZDRawMouse RUNTIME DESTINATION .)
install(FILES config/HZDRawMouse.ini DESTINATION .)
