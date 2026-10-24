@echo off
rem ===========================================================================
rem build_launcher.bat -- CMake compiler launcher for Windows/MSYS2.
rem
rem CMake invokes this as:  build_launcher.bat <compiler> <args...>
rem
rem WHAT IT DOES: routes every compile through MSYS2's bash, which runs
rem scripts/cc_retry.sh (the retry wrapper). Two problems are handled there:
rem
rem   (1) PATH form. Ninja on Windows runs compile commands through cmd.exe.
rem       MSYS's runtime translates PATH when handing the environment to a
rem       native program, so cmd.exe sees a Windows-style PATH. Under that
rem       PATH the gcc child process gets a broken environment and dies at
rem       startup. bash (an MSYS program) keeps the POSIX-style PATH.
rem       -> handled by running the compile from inside bash.
rem
rem   (2) Defender. MsMpEng (real-time protection) scans newly started
rem       cc1plus.exe and sometimes kills it; gcc then exits with code 1 and
rem       NOT A SINGLE BYTE on stderr. Looks like "code does not compile"
rem       but the code is fine. -> handled by scripts/cc_retry.sh, which
rem       retries when stderr is empty.
rem
rem   IMPORTANT: keep this file ASCII-only. cmd.exe mis-parses non-ASCII
rem   bytes in .bat files (comment text gets executed as commands).
rem   Chinese notes live in scripts/build_win.sh.
rem
rem HOW TO PASS ARGS: `sh -c "%*"` would re-parse the whole command line as
rem   one string, breaking on spaces and non-ASCII. Instead we pass the
rem   compiler and its arguments as sh positional parameters and let the
rem   script use "$@" so argument boundaries survive intact.
rem ===========================================================================
setlocal
set "MSYS_SH=D:\msys64\usr\bin\sh.exe"
set "RETRY_SH=E:\navimap\scripts\cc_retry.sh"
if not exist "%MSYS_SH%" (
    echo build_launcher.bat: cannot find %MSYS_SH% 1>&2
    exit /b 2
)
if not exist "%RETRY_SH%" (
    echo build_launcher.bat: cannot find %RETRY_SH% 1>&2
    exit /b 2
)
rem Run the compile through the retry wrapper (scripts/cc_retry.sh), which is
rem a bash script: it retries when the compiler dies with an empty stderr
rem (Defender killed cc1plus) and gives up immediately on a real error.
rem Passing %* as-is keeps argument boundaries intact (no re-parsing).
"%MSYS_SH%" "%RETRY_SH%" %*
exit /b %ERRORLEVEL%
