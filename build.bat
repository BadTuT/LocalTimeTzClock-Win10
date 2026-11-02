@echo off
setlocal
REM LocalTimeTzClock build script.
REM   build.bat [outdir]      default outdir: dist
REM
REM DLLNAME is versioned on purpose: a DLL already loaded into explorer.exe
REM cannot be overwritten, so a fresh file name lets us rebuild without
REM restarting the user's Explorer. Bump it whenever the DLL changes.
set OUTDIR=%~1
if "%OUTDIR%"=="" set OUTDIR=dist
set DLLNAME=tzclock_v13.dll

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set CFLAGS=/nologo /c /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MD /utf-8
set LDFLAGS=/nologo /RELEASE /OPT:REF /OPT:ICF /INCREMENTAL:NO

cl %CFLAGS% /Fo:"%OUTDIR%\tzclock_dll.obj" src\dll\tzclock.cpp
if errorlevel 1 exit /b 1
link %LDFLAGS% /DLL /OUT:"%OUTDIR%\%DLLNAME%" "%OUTDIR%\tzclock_dll.obj" ^
  kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

cl %CFLAGS% /DTZCLOCK_DLL_NAME="L\"%DLLNAME%\"" /Fo:"%OUTDIR%\tzclock_app.obj" src\app\tzclock.cpp
if errorlevel 1 exit /b 1
link %LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUTDIR%\tzclock.exe" "%OUTDIR%\tzclock_app.obj" ^
  kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

copy /y tzclock.ini "%OUTDIR%\tzclock.ini" >nul
echo BUILD_OK (%DLLNAME%)
