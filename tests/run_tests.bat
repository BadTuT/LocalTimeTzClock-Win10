@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

set OUTDIR=%TEMP%\tzclock_test
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

cl /nologo /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MD /utf-8 /I. tests\regression_tests.cpp /Fo"%OUTDIR%\regression_tests.obj" /Fe"%OUTDIR%\regression_tests.exe" /link /INCREMENTAL:NO kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 ( echo COMPILE_FAIL & exit /b 1 )

"%OUTDIR%\regression_tests.exe"
if errorlevel 1 exit /b 1

cl /nologo /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MD /utf-8 tests\controller_tests.cpp /Fo"%OUTDIR%\controller_tests.obj" /Fe"%OUTDIR%\controller_tests.exe" /link /INCREMENTAL:NO kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1
"%OUTDIR%\controller_tests.exe"
if errorlevel 1 exit /b 1

cl /nologo /O1 /Os /GS- /GR- /EHs-c- /DNDEBUG /W3 /MD /utf-8 tests\snapshot_tests.cpp /Fo"%OUTDIR%\snapshot_tests.obj" /Fe"%OUTDIR%\snapshot_tests.exe" /link /INCREMENTAL:NO kernel32.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 exit /b 1
"%OUTDIR%\snapshot_tests.exe"
exit /b %errorlevel%
