#!/bin/sh
# Local MSVC / CMake / Ninja environment for this machine (VS 2026 Community +
# Windows SDK on D:).  Sourced by build commands; not part of the project.
VS_BIN="/c/Program Files/Microsoft Visual Studio/18/Community"
MSVC_WIN='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
SDK_WIN='D:\Windows Kits\10'
SDK_VER=10.0.26100.0

export PATH="$VS_BIN/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64:$VS_BIN/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$VS_BIN/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:/d/Windows Kits/10/bin/$SDK_VER/x64:$PATH"
export INCLUDE="$MSVC_WIN\\include;$SDK_WIN\\Include\\$SDK_VER\\ucrt;$SDK_WIN\\Include\\$SDK_VER\\um;$SDK_WIN\\Include\\$SDK_VER\\shared;$SDK_WIN\\Include\\$SDK_VER\\winrt"
export LIB="$MSVC_WIN\\lib\\x64;$SDK_WIN\\Lib\\$SDK_VER\\ucrt\\x64;$SDK_WIN\\Lib\\$SDK_VER\\um\\x64"
export VCPKG_ROOT=/e/sdk/vcpkg
export CMAKE_TOOLCHAIN_FILE='E:/sdk/vcpkg/scripts/buildsystems/vcpkg.cmake'
# CMakePresets.json expands $env{VSINSTALLDIR}; it is only set inside a VS
# developer prompt, so provide it here (trailing backslash is intentional).
export VSINSTALLDIR='C:\Program Files\Microsoft Visual Studio\18\Community\'
