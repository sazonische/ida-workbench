@echo off
REM Configure + build the Qt GUI, then assemble a self-contained final build in dist\.
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "QTDIR=C:\Qt\6.8.3\msvc2022_64"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=%CMAKE%;%NINJA%;%PATH%"

cd /d "%~dp0"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" || exit /b 1
cmake --build build || exit /b 1

REM ---- assemble dist\ : exe + Qt runtime + python glue ----
set "DIST=%~dp0dist"
if not exist "%DIST%" mkdir "%DIST%"
copy /y "build\ida-workbench.exe" "%DIST%\" >nul || exit /b 1
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime "%DIST%\ida-workbench.exe" >nul || exit /b 1
copy /y "%~dp0start_mcp.py"          "%DIST%\" >nul || exit /b 1
copy /y "%~dp0analyze_ida.py"        "%DIST%\" >nul || exit /b 1
copy /y "%~dp0disable_autostart.py"  "%DIST%\" >nul || exit /b 1

echo.
echo BUILD_OK : %DIST%\ida-workbench.exe
endlocal
