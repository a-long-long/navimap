@echo off
rem cc_wrap.bat -- route C compiler through MSYS bash so CMake's
rem compiler detection survives the cmd.exe PATH-translation bug
rem (see build_win.sh / build_launcher.bat). CMake calls this as
rem the CMAKE_C_COMPILER:  cc_wrap.bat <args...>
setlocal
set "CC=D:\msys64\ucrt64\bin\cc.exe"
set "MSYS_SH=D:\msys64\usr\bin\sh.exe"
set "RETRY_SH=E:\navimap\scripts\cc_retry.sh"
if not exist "%CC%" ( echo cc_wrap: cannot find %CC% 1>&2 & exit /b 2 )
if not exist "%MSYS_SH%" ( echo cc_wrap: cannot find %MSYS_SH% 1>&2 & exit /b 2 )
if not exist "%RETRY_SH%" ( echo cc_wrap: cannot find %RETRY_SH% 1>&2 & exit /b 2 )
"%MSYS_SH%" "%RETRY_SH%" "%CC%" %*
exit /b %ERRORLEVEL%
