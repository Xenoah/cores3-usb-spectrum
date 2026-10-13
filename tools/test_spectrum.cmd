@echo off
setlocal
where cl >nul 2>nul
if errorlevel 1 (
    echo Run this script from an x64 Native Tools Command Prompt for Visual Studio.
    exit /b 1
)
pushd "%~dp0.."
cl /nologo /EHsc /std:c++17 /O2 /Fe:tools\test_spectrum.exe /Fo:tools\test_spectrum.obj tools\test_spectrum.cpp
if errorlevel 1 (
    popd
    exit /b 1
)
tools\test_spectrum.exe
set "test_result=%errorlevel%"
popd
exit /b %test_result%
