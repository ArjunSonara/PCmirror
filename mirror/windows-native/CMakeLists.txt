cmake_minimum_required(VERSION 3.20)
project(PCMirror LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "This project uses DXGI/Media Foundation and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(PCMirror
    src/main.cpp
    src/capture.cpp
    src/encoder.cpp
    src/network.cpp
    src/input_server.cpp
    src/audio_server.cpp
    src/autodetect.cpp
)

if (MSVC)
    target_compile_options(PCMirror PRIVATE /arch:AVX2 /O2 /openmp)
endif()

target_link_libraries(PCMirror PRIVATE
    d3d11.lib
    dxgi.lib
    mfplat.lib
    mf.lib
    mfuuid.lib
    ws2_32.lib
    iphlpapi.lib
    user32.lib
    gdi32.lib
    ole32.lib
    oleaut32.lib
)
