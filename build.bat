@echo off
rem Builds build\stick_fix.dll with the MSVC Build Tools (x64).
rem Set VCVARS to your vcvars64.bat if it isn't in the default place.
setlocal
if "%VCVARS%"=="" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo vcvars64.bat not found at "%VCVARS%". Set VCVARS, or use CMake.
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /LD /Iinclude src\stick_fix.cpp /Fobuild\ /Febuild\stick_fix.dll /link /NOLOGO || exit /b 1
echo Built build\stick_fix.dll
