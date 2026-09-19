@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"
cl /nologo /O2 /arch:AVX2 /openmp /std:c++17 /EHsc src/main.cpp src/capture.cpp src/encoder.cpp src/network.cpp src/input_server.cpp src/audio_server.cpp src/autodetect.cpp /link /out:PCMirror.exe d3d11.lib dxgi.lib mfplat.lib mf.lib mfuuid.lib ws2_32.lib iphlpapi.lib user32.lib gdi32.lib ole32.lib oleaut32.lib avrt.lib
