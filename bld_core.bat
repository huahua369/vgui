@echo off
rem VGUI_CORE_ONLY build: core library without the SDL3 backend + headless tests.
rem See docs/runtime_roadmap.md (R0).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not exist out\build\x64-core\CMakeCache.txt (
  cmake -S . -B out\build\x64-core -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_TOOLCHAIN_FILE=E:/sdk/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_INSTALLED_DIR=%CD%\out\build\x64-debug\vcpkg_installed -DOVG_ENABLE_SAMPLE=OFF -DVGUI_CORE_ONLY=ON
)
ninja -C out\build\x64-core %*
