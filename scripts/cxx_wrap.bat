@echo off
rem cxx_wrap.bat -- see cc_wrap.bat. CMake calls this as the
rem CMAKE_CXX_COMPILER:  cxx_wrap.bat <args...>
setlocal
set "CXX=D:\msys64\ucrt64\bin\c++.exe"
set "MSYS_SH=D:\msys64\usr\bin\sh.exe"
set "RETRY_SH=E:\navimap\scripts\cc_retry.sh"
if not exist "%CXX%" ( echo cxx_wrap: cannot find %CXX% 1>&2 & exit /b 2 )
if not exist "%MSYS_SH%" ( echo cxx_wrap: cannot find %MSYS_SH% 1>&2 & exit /b 2 )
if not exist "%RETRY_SH%" ( echo cxx_wrap: cannot find %RETRY_SH% 1>&2 & exit /b 2 )
"%MSYS_SH%" "%RETRY_SH%" "%CXX%" %*
exit /b %ERRORLEVEL%
