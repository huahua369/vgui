@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
rem Keep the same /showIncludes language as bld_full.bat.
set "VSLANG=1033"
ninja -C out\build\x64-debug %*
