@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )
if not exist build mkdir build

cl /nologo /c /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MT /Fo:build\tzc_hook.obj src\dll\tzc_hook.cpp
if errorlevel 1 exit /b 1
link /nologo /DLL /RELEASE /OPT:REF /OPT:ICF /INCREMENTAL:NO /OUT:build\tzclock.dll build\tzc_hook.obj kernel32.lib user32.lib advapi32.lib shell32.lib version.lib shlwapi.lib
if errorlevel 1 exit /b 1

cl /nologo /c /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MT /Fo:build\inject.obj src\inject\inject.cpp
if errorlevel 1 exit /b 1
link /nologo /SUBSYSTEM:CONSOLE /RELEASE /OPT:REF /OPT:ICF /INCREMENTAL:NO /OUT:build\tzclock-inject.exe build\inject.obj kernel32.lib user32.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
