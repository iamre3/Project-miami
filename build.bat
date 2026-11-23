@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo [ERROR] Failed to initialize MSVC x64 developer environment.
    exit /b 1
)

set "MIAMI_ROOT=e:\ProjectMiami"
set "XENITH_ROOT=e:\XENITH"
set "LEONIDA_ROOT=e:\XENITH\references\Leonida"
set "COMPAT_ROOT=e:\XENITH\references\GTA6-COMPAT-LAYER"
set "OUT=%MIAMI_ROOT%\out"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

echo.
echo [1/5] Compiling GTA6-COMPAT-LAYER (ScarlettCompat.dll + Shim Import Libraries)...
cl /nologo /EHsc /MD /O2 /W3 /utf-8 ^
   /I"%COMPAT_ROOT%\include" ^
   "%COMPAT_ROOT%\src\scarlett.cpp" ^
   "%COMPAT_ROOT%\src\pe_loader.cpp" ^
   "%COMPAT_ROOT%\src\hle_registry.cpp" ^
   /LD /Fe:"%OUT%\ScarlettCompat.dll" /Fo"%OUT%\obj\\"
if errorlevel 1 (
    echo [ERROR] Failed to compile ScarlettCompat.dll
    exit /b 1
)
for %%D in (xgameruntime xmem d3d12_xs XAudio2_9 dstorage_xs gameinput) do (
    lib /nologo /MACHINE:X64 /DEF:"%COMPAT_ROOT%\defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)
echo [OK] GTA6-COMPAT-LAYER ScarlettCompat.dll and 6 shim .lib files built in %OUT%

set XENITH_SRCS="%XENITH_ROOT%\src\core\core.cpp" "%XENITH_ROOT%\src\core\backend_api.cpp" "%XENITH_ROOT%\src\cpu\cpu.cpp" "%XENITH_ROOT%\src\memory\memory.cpp" "%XENITH_ROOT%\src\gpu\gpu.cpp" "%XENITH_ROOT%\src\system\system.cpp" "%XENITH_ROOT%\src\input\input.cpp" "%XENITH_ROOT%\src\audio\audio.cpp" "%XENITH_ROOT%\src\storage\storage.cpp" "%XENITH_ROOT%\src\compat\compat.cpp" "%XENITH_ROOT%\src\loader\loader.cpp" "%XENITH_ROOT%\src\debugger\debugger.cpp"
set LEONIDA_SRCS="%LEONIDA_ROOT%\runtime\src\root_signature.cpp" "%LEONIDA_ROOT%\runtime\shims\d3d12_xs_rootsig.cpp"
set MIAMI_INCLUDES=/I"%MIAMI_ROOT%\include" /I"%XENITH_ROOT%\include" /I"%COMPAT_ROOT%\include" /I"%COMPAT_ROOT%\src" /I"%LEONIDA_ROOT%\runtime\src" /I"%LEONIDA_ROOT%\runtime\shims"

echo.
echo [2/5] Compiling ProjectMiami.exe (Linked with XENITH + Leonida + GTA6-COMPAT-LAYER)...
cl /nologo /O2 /W3 /std:c++20 /EHa /MD /utf-8 /DNOMINMAX ^
   %MIAMI_INCLUDES% ^
   "%MIAMI_ROOT%\src\core\app.cpp" ^
   "%MIAMI_ROOT%\src\core\main.cpp" ^
   "%MIAMI_ROOT%\src\core\library.cpp" ^
   "%MIAMI_ROOT%\src\core\integration.cpp" ^
   "%MIAMI_ROOT%\src\config\config.cpp" ^
   "%MIAMI_ROOT%\src\install\installer.cpp" ^
   "%MIAMI_ROOT%\src\process\process.cpp" ^
   "%MIAMI_ROOT%\src\ui\renderer.cpp" ^
   %XENITH_SRCS% ^
   %LEONIDA_SRCS% ^
   /Fe:"%OUT%\ProjectMiami.exe" /Fo"%OUT%\obj\\" ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib windowscodecs.lib dxgi.lib d3d12.lib d3dcompiler.lib xinput.lib winmm.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile ProjectMiami.exe
    exit /b 1
)
echo [OK] ProjectMiami.exe compiled and linked with XENITH, Leonida, and GTA6-COMPAT-LAYER

echo.
echo [3/5] Compiling test_miami.exe (Automated Verification Suite)...
cl /nologo /O2 /W3 /std:c++20 /EHa /MD /utf-8 /DNOMINMAX ^
   %MIAMI_INCLUDES% ^
   "%MIAMI_ROOT%\tests\test_miami.cpp" ^
   "%MIAMI_ROOT%\src\core\app.cpp" ^
   "%MIAMI_ROOT%\src\core\library.cpp" ^
   "%MIAMI_ROOT%\src\core\integration.cpp" ^
   "%MIAMI_ROOT%\src\config\config.cpp" ^
   "%MIAMI_ROOT%\src\install\installer.cpp" ^
   "%MIAMI_ROOT%\src\process\process.cpp" ^
   %XENITH_SRCS% ^
   %LEONIDA_SRCS% ^
   /Fe:"%OUT%\test_miami.exe" /Fo"%OUT%\obj\\" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dxgi.lib d3d12.lib d3dcompiler.lib xinput.lib winmm.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile test_miami.exe
    exit /b 1
)
echo [OK] test_miami.exe generated at %OUT%\test_miami.exe

echo.
echo [4/5] Executing Automated Verification Suite...
"%OUT%\test_miami.exe"
if errorlevel 1 (
    echo [ERROR] Verification suite failed!
    exit /b 1
)

echo.
echo ====================================================================
echo  PROJECT MIAMI + XENITH + LEONIDA + GTA6-COMPAT-LAYER VERIFIED!
echo ====================================================================
exit /b 0
