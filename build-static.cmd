@echo off
REM Build a SINGLE self-contained exe using the static Qt from vcpkg.
REM Install that Qt first, with EXACTLY these features (`core` = no default features):
REM   vcpkg install "qtbase[core,doubleconversion,freetype,gui,harfbuzz,network,pcre2,png,thread,widgets]:x64-windows-static"
REM Plain `vcpkg install qtbase:x64-windows-static` adds icu, openssl, postgresql, dbus,
REM sql and testlib: hours of extra port builds and a 58 MB exe instead of 22 MB. Nothing
REM is lost — the app links only Widgets and Network, and HTTPS uses Windows schannel.
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "VCPKG=C:\src\vcpkg"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=%CMAKE%;%NINJA%;%PATH%"

cd /d "%~dp0"
cmake -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static || exit /b 1
cmake --build build-static || exit /b 1

REM ---- assemble dist-static\ : exe + python glue ----
set "DIST=%~dp0dist-static"
if not exist "%DIST%" mkdir "%DIST%"
copy /y "build-static\ida-workbench.exe" "%DIST%\ida-workbench.exe" >nul || exit /b 1
copy /y "%~dp0start_mcp.py"          "%DIST%\start_mcp.py" >nul || exit /b 1
copy /y "%~dp0analyze_ida.py"        "%DIST%\analyze_ida.py" >nul || exit /b 1
copy /y "%~dp0disable_autostart.py"  "%DIST%\disable_autostart.py" >nul || exit /b 1

echo.
echo STATIC_BUILD_OK : %DIST%\ida-workbench.exe
endlocal
