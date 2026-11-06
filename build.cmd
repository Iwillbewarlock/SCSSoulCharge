@echo off
rem SCSSoulCharge - configure + Release build + offline tests.
rem Usage: build.cmd          (incremental)
rem        build.cmd repair   (wipe the build folder and reconfigure)
rem Build output lives on D: (C: is nearly full): D:\DEV\build\SCSSoulCharge\release
setlocal EnableDelayedExpansion

set "ROOT=%~dp0"
set "BUILD=D:\DEV\build\SCSSoulCharge\release"
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\Dev\vcpkg"

if not exist "%ROOT%extern\CommonLibSSE-NG\CMakeLists.txt" (
    echo ERROR: extern\CommonLibSSE-NG missing. Clone alandtse/CommonLibVR at 39f9d07 there.
    exit /b 1
)

set "VCVARS="
for %%V in (18 2022) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
            if not defined VCVARS (
                if exist "%%~P\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
                    set "VCVARS=%%~P\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
                )
            )
        )
    )
)
if not defined VCVARS (
    echo ERROR: no Visual Studio vcvars64.bat found.
    exit /b 1
)
call "%VCVARS%" >nul || (echo ERROR: vcvars64 failed.& exit /b 1)

cd /d "%ROOT%"

if /i "%~1"=="repair" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if exist "%BUILD%\CMakeCache.txt" (
    set "HOME=%ROOT:~0,-1%"
    set "HOME=!HOME:\=/!"
    findstr /i /c:"CMAKE_HOME_DIRECTORY:INTERNAL=!HOME!" "%BUILD%\CMakeCache.txt" >nul || (
        echo Cache belongs to another folder - deleting it and reconfiguring.
        rmdir /s /q "%BUILD%"
    )
)

if not exist "%BUILD%\CMakeCache.txt" (
    cmake --preset release "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=D:/DEV/build/vcpkg-buildtrees;--x-packages-root=D:/DEV/build/vcpkg-packages" || exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
cmake --build "%BUILD%" > "%BUILD%\build.log" 2>&1
if errorlevel 1 (
    echo BUILD FAILED. Error lines:
    findstr /i /r /c:"error" "%BUILD%\build.log"
    exit /b 1
)

"%BUILD%\core_tests.exe" || (echo TESTS FAILED& exit /b 1)

echo BUILD-OK: %BUILD%\SCSSoulCharge.dll
endlocal
exit /b 0
