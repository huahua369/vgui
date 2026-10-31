@echo off
rem Full build (SDL3 backend), mirrors bld.bat but also works without a VS dev shell.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not exist out\build\x64-debug\CMakeCache.txt (
  cmake -S . -B out\build\x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_TOOLCHAIN_FILE=E:/sdk/vcpkg/scripts/buildsystems/vcpkg.cmake -DOVG_ENABLE_SAMPLE=ON
)
ninja -C out\build\x64-debug %*
