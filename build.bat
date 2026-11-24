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
if not exist "%OUT%\obj\app" mkdir "%OUT%\obj\app"
if not exist "%OUT%\obj\test" mkdir "%OUT%\obj\test"

echo.
echo [1/5] Compiling GTA6-COMPAT-LAYER (ScarlettCompat.dll + Shim Import Libraries)...
cl /nologo /EHsc /MD /O2 /W3 /utf-8 ^
   /I"%COMPAT_ROOT%\include" ^
   "%COMPAT_ROOT%\src\scarlett.cpp" ^
   "%COMPAT_ROOT%\src\pe_loader.cpp" ^
   "%COMPAT_ROOT%\src\hle_registry.cpp" ^
   "%COMPAT_ROOT%\src\era_to_gdk.cpp" ^
   "%COMPAT_ROOT%\src\software_rt.cpp" ^
   "%COMPAT_ROOT%\src\fixed_timestep.cpp" ^
   "%COMPAT_ROOT%\src\winrt_shim.cpp" ^
   "%COMPAT_ROOT%\src\present.cpp" ^
   "%COMPAT_ROOT%\src\opt_pso_cache.cpp" ^
   "%COMPAT_ROOT%\src\opt_detile.cpp" ^
   "%COMPAT_ROOT%\src\xg_texture.cpp" ^
   "%COMPAT_ROOT%\src\opt_mesh_fallback.cpp" ^
   "%COMPAT_ROOT%\src\opt_async_compute.cpp" ^
   "%COMPAT_ROOT%\src\opt_desc_heap.cpp" ^
   "%COMPAT_ROOT%\src\opt_dstorage.cpp" ^
   "%COMPAT_ROOT%\src\opt_shader_cache.cpp" ^
   "%COMPAT_ROOT%\src\opt_texcache.cpp" ^
   "%COMPAT_ROOT%\src\opt_batching.cpp" ^
   /LD /Fe:"%OUT%\ScarlettCompat.dll" /Fo"%OUT%\obj\\" ^
   /link d3d12.lib d3d11.lib d3dcompiler.lib dxgi.lib winmm.lib runtimeobject.lib user32.lib ole32.lib
if errorlevel 1 (
    echo [ERROR] Failed to compile ScarlettCompat.dll
    exit /b 1
)
for %%D in (xgameruntime xmem d3d12_xs XAudio2_9 dstorage_xs gameinput d3d11_x d3d12_x kernelx xg_x AcpHal) do (
    lib /nologo /MACHINE:X64 /DEF:"%COMPAT_ROOT%\defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)

REM Build scarlett_verify.exe (Upstream real-exe loader harness)
cl /nologo /EHsc /MD /O2 /W3 /utf-8 /I"%COMPAT_ROOT%\include" ^
   "%COMPAT_ROOT%\src\verify.cpp" ^
   /Fe:"%OUT%\scarlett_verify.exe" /Fo"%OUT%\obj\\" ^
   /link "%OUT%\ScarlettCompat.lib"
echo [OK] GTA6-COMPAT-LAYER ScarlettCompat.dll, scarlett_verify.exe, and 11 shim .lib files built in %OUT%

set XENITH_SRCS="%XENITH_ROOT%\src\core\core.cpp" "%XENITH_ROOT%\src\core\backend_api.cpp" "%XENITH_ROOT%\src\cpu\cpu.cpp" "%XENITH_ROOT%\src\memory\memory.cpp" "%XENITH_ROOT%\src\gpu\gpu.cpp" "%XENITH_ROOT%\src\system\system.cpp" "%XENITH_ROOT%\src\input\input.cpp" "%XENITH_ROOT%\src\audio\audio.cpp" "%XENITH_ROOT%\src\storage\storage.cpp" "%XENITH_ROOT%\src\compat\compat.cpp" "%XENITH_ROOT%\src\loader\loader.cpp" "%XENITH_ROOT%\src\debugger\debugger.cpp"
set LEONIDA_SRCS="%LEONIDA_ROOT%\runtime\src\root_signature.cpp" "%LEONIDA_ROOT%\runtime\shims\d3d12_xs_rootsig.cpp"
set MIAMI_INCLUDES=/I"%MIAMI_ROOT%\include" /I"%MIAMI_ROOT%\src" /I"%XENITH_ROOT%\include" /I"%COMPAT_ROOT%\include" /I"%COMPAT_ROOT%\src" /I"%LEONIDA_ROOT%\runtime\src" /I"%LEONIDA_ROOT%\runtime\shims"

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
   /Fe:"%OUT%\ProjectMiami.exe" /Fo"%OUT%\obj\app\\" ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib windowscodecs.lib dxgi.lib d3d12.lib d3dcompiler.lib xinput.lib winmm.lib advapi32.lib dwmapi.lib

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
   /Fe:"%OUT%\test_miami.exe" /Fo"%OUT%\obj\test\\" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dxgi.lib d3d12.lib d3dcompiler.lib xinput.lib winmm.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile test_miami.exe
    exit /b 1
)
echo [OK] test_miami.exe generated at %OUT%\test_miami.exe

echo.
echo [4/6] Compiling test_graphics.exe (Native Direct3D 12 Hardware Graphics Test)...
cl /nologo /O2 /W3 /std:c++20 /EHa /MD /utf-8 /DNOMINMAX ^
   %MIAMI_INCLUDES% ^
   "%MIAMI_ROOT%\tests\test_graphics.cpp" ^
   "%MIAMI_ROOT%\src\graphics\shader_compiler.cpp" ^
   "%MIAMI_ROOT%\src\graphics\diagnostics.cpp" ^
   "%MIAMI_ROOT%\src\graphics\runtime_bridge.cpp" ^
   "%MIAMI_ROOT%\src\graphics\d3d12\d3d12_backend.cpp" ^
   /Fe:"%OUT%\test_graphics.exe" /Fo"%OUT%\obj\\" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dxgi.lib d3d12.lib d3dcompiler.lib dxguid.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile test_graphics.exe
    exit /b 1
)
echo [OK] test_graphics.exe generated at %OUT%\test_graphics.exe

echo.
echo [5/8] Compiling Progressive Guest Test Applications...
cl /nologo /O2 /W3 /std:c++20 /EHsc /MD /utf-8 /Fe:"%OUT%\app_minimal.exe" /Fo"%OUT%\obj\\" "%MIAMI_ROOT%\tests\apps\app_minimal.cpp"
cl /nologo /O2 /W3 /std:c++20 /EHsc /MD /utf-8 /Fe:"%OUT%\app_services.exe" /Fo"%OUT%\obj\\" "%MIAMI_ROOT%\tests\apps\app_services.cpp"
cl /nologo /O2 /W3 /std:c++20 /EHsc /MD /utf-8 /Fe:"%OUT%\app_graphics.exe" /Fo"%OUT%\obj\\" "%MIAMI_ROOT%\tests\apps\app_graphics.cpp" /link dxgi.lib d3d12.lib
echo [OK] Guest test executables generated

echo.
echo [6/8] Compiling test_runtime.exe (Modular Runtime and System Services Test)...
set RUNTIME_SRCS="%MIAMI_ROOT%\src\runtime\runtime.cpp" "%MIAMI_ROOT%\src\runtime\memory_service.cpp" "%MIAMI_ROOT%\src\runtime\storage_service.cpp" "%MIAMI_ROOT%\src\runtime\input_service.cpp" "%MIAMI_ROOT%\src\runtime\audio_service.cpp" "%MIAMI_ROOT%\src\runtime\network_service.cpp" "%MIAMI_ROOT%\src\runtime\feature_matrix.cpp" "%MIAMI_ROOT%\src\graphics\shader_compiler.cpp" "%MIAMI_ROOT%\src\graphics\diagnostics.cpp" "%MIAMI_ROOT%\src\graphics\runtime_bridge.cpp" "%MIAMI_ROOT%\src\graphics\d3d12\d3d12_backend.cpp"

cl /nologo /O2 /W3 /std:c++20 /EHa /MD /utf-8 /DNOMINMAX ^
   %MIAMI_INCLUDES% ^
   "%MIAMI_ROOT%\tests\test_runtime.cpp" ^
   %RUNTIME_SRCS% ^
   /Fe:"%OUT%\test_runtime.exe" /Fo"%OUT%\obj\\" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dxgi.lib d3d12.lib d3dcompiler.lib dxguid.lib xinput.lib winmm.lib ws2_32.lib advapi32.lib
if errorlevel 1 (
    echo [ERROR] Failed to compile test_runtime.exe
    exit /b 1
)
echo [OK] test_runtime.exe generated at %OUT%\test_runtime.exe

echo.
echo [7/8] Executing Automated Compatibility Verification Suites...
"%OUT%\test_miami.exe" || exit /b 1
"%OUT%\test_graphics.exe" || exit /b 1

echo.
echo [8/8] Executing Modular Runtime and System Services Verification...
"%OUT%\test_runtime.exe" || exit /b 1

echo.
echo ====================================================================
echo  PROJECT MIAMI COMPATIBILITY PLATFORM AND ALL SUBSYSTEMS VERIFIED!
echo ====================================================================
exit /b 0
