@echo off
setlocal
REM LocalTimeTzClock build script.
REM Output directory is the first argument, defaults to "dist".

set OUTDIR=%~1
if "%OUTDIR%"=="" set OUTDIR=dist

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set CFLAGS=/nologo /c /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MT /utf-8
set LDFLAGS=/nologo /RELEASE /OPT:REF /OPT:ICF /INCREMENTAL:NO

cl %CFLAGS% /Fo:"%OUTDIR%\tzclock_dll.obj" src\dll\tzclock.cpp
if errorlevel 1 exit /b 1
link %LDFLAGS% /DLL /OUT:"%OUTDIR%\tzclock.dll" "%OUTDIR%\tzclock_dll.obj" ^
  kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

cl %CFLAGS% /Fo:"%OUTDIR%\tzclock_app.obj" src\app\tzclock.cpp
if errorlevel 1 exit /b 1
link %LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%OUTDIR%\tzclock.exe" "%OUTDIR%\tzclock_app.obj" ^
  kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1

copy /y tzclock.ini "%OUTDIR%\tzclock.ini" >nul
echo BUILD_OK
