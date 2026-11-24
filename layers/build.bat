@echo off
REM ScarlettCompat build — Visual Studio 2022 BuildTools + Windows SDK 10.0.26100.0
REM Builds: layer DLL, shim import libs, synthetic guest title, verify harness,
REM then runs the harness against the guest. No game code is built here and
REM nothing in this script touches D:\game\game.
REM RUN THIS IN AN INTERACTIVE SESSION (session 1). The scheduled task
REM ScarlettPhase5Build is created with schtasks /it for exactly this reason.
REM In session 0 (SYSTEM, non-interactive) the guests crash after the TLS
REM stage: EXCEPTION_ACCESS_VIOLATION in gdi32full.dll+0x8984a, reading
REM address 0x20 — a NULL+0x20 dereference on a thread whose stack holds
REM the AMD driver (amdxc64), RTSSHooks64, combase/imm32. The layer never
REM calls GDI; it is the session-0 window station + graphics stack, not a
REM layer bug. Proven: the same binaries pass ALL PASS in session 1 and in
REM a foreground SSH run, and crash 100% in session 0. (Phase 4, 2026-11-23.)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"

echo === 1. ScarlettCompat.dll (layer) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\scarlett.cpp" "%ROOT%src\pe_loader.cpp" "%ROOT%src\hle_registry.cpp" "%ROOT%src\era_to_gdk.cpp" "%ROOT%src\software_rt.cpp" "%ROOT%src\fixed_timestep.cpp" "%ROOT%src\winrt_shim.cpp" "%ROOT%src\present.cpp" "%ROOT%src\opt_pso_cache.cpp" "%ROOT%src\opt_detile.cpp" "%ROOT%src\xg_texture.cpp" "%ROOT%src\opt_mesh_fallback.cpp" "%ROOT%src\opt_async_compute.cpp" "%ROOT%src\opt_desc_heap.cpp" "%ROOT%src\opt_dstorage.cpp" "%ROOT%src\opt_shader_cache.cpp" "%ROOT%src\opt_texcache.cpp" "%ROOT%src\opt_batching.cpp" /LD /Fe"%OUT%\ScarlettCompat.dll" /Fo%OUT%\ /Fd%OUT%\ScarlettCompat.pdb /link d3d12.lib d3d11.lib d3dcompiler.lib dxgi.lib winmm.lib runtimeobject.lib user32.lib ole32.lib
if errorlevel 1 exit /b 1

echo === 2. Shim import libs (Xbox DLL names mapped to layer exports) ===
for %%D in (xgameruntime xmem d3d12_xs XAudio2_9 dstorage_xs gameinput d3d11_x d3d12_x kernelx xg_x AcpHal) do (
  lib /nologo /DEF:"%ROOT%defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)
REM Ordinal import libs (TEST ONLY — synthetic ordinals for the ordinal guest).
for %%D in (xgameruntime_ord xmem_ord) do (
  lib /nologo /DEF:"%ROOT%defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)
REM Real-DLL import lib for the WinRT proof guest only: vccorlib110.lib
REM lets guest_winrt.cpp import GetActivationFactoryByPCWSTR by its
REM MANGLED name (the only export the real vccorlib110.dll has for it)
REM from the real vccorlib110.dll (VS2012 redist). NOT a shim — the
REM layer never links it.
lib /nologo /DEF:"%ROOT%defs\vccorlib110.def" /OUT:"%OUT%\lib\vccorlib110.lib" || exit /b 1

echo === 3. Synthetic guest title (Xbox-style PE) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_minimal.cpp" /Fe"%OUT%\guest_minimal.exe" /Fo%OUT%\ /Fd%OUT%\guest_minimal.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib xmem.lib d3d12_xs.lib XAudio2_9.lib dstorage_xs.lib gameinput.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
if not exist "%OUT%\ordinal" mkdir "%OUT%\ordinal"
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_ordinal.cpp" /Fe"%OUT%\ordinal\guest_ordinal.exe" /Fo%OUT%\ /Fd%OUT%\guest_ordinal.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime_ord.lib xmem_ord.lib
if errorlevel 1 exit /b 1
REM Phase 4: realistic proof guest (full boot + triangle + 60-frame loop).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_proof.cpp" /Fe"%OUT%\guest_proof.exe" /Fo%OUT%\ /Fd%OUT%\guest_proof.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib xmem.lib d3d12_xs.lib XAudio2_9.lib dstorage_xs.lib gameinput.lib d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
REM Phase 5: ERA proof guest (Xbox One-shaped boot through the ERA -> GDK
REM translation: ERA graphics/memory/audio + WinRT routing + GDK services).
REM NOTE: xmem.lib is NOT linked — kernelx.lib already exports XMemAlloc /
REM XMemFree, and linking both would duplicate the import symbol. The
REM guest uses D3D11X/D3D12X device creation, so d3d12_xs.lib is not
REM linked either; d3d12.lib stays for D3D12SerializeRootSignature.
REM xg_x.lib + AcpHal.lib are linked for the era_noequiv stage (the
REM guest calls one entry of each and expects documented E_NOTIMPL).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_era.cpp" /Fe"%OUT%\guest_era.exe" /Fo%OUT%\ /Fd%OUT%\guest_era.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" d3d11_x.lib d3d12_x.lib kernelx.lib xg_x.lib AcpHal.lib xgameruntime.lib XAudio2_9.lib d3d12.lib d3dcompiler.lib runtimeobject.lib xinput.lib winmm.lib
if errorlevel 1 exit /b 1
REM Software ray tracing proof guest (compute ray tracer through the
REM layer's ScSoftwareRT_* entry points, pixel-verified on the 5700 XT).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_software_rt.cpp" /Fe"%OUT%\guest_software_rt.exe" /Fo%OUT%\ /Fd%OUT%\guest_software_rt.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib d3d12_xs.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
REM Optimization proof guest (one stage per optimization item in
REM docs/OPTIMIZATION.md, each reporting measured before/after numbers).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_opt.cpp" /Fe"%OUT%\guest_opt.exe" /Fo%OUT%\ /Fd%OUT%\guest_opt.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib d3d12_xs.lib d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
REM Fixed-timestep physics proof guest (physics at a fixed 60 Hz
REM while rendering at 30 and 120 FPS; see docs/FIXED-TIMESTEP.md).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_physics.cpp" /Fe"%OUT%\guest_physics.exe" /Fo%OUT%\ /Fd%OUT%\guest_physics.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib d3d12_xs.lib winmm.lib
if errorlevel 1 exit /b 1
REM WinRT shim proof guest (activation via vccorlib110 + direct RoGet,
REM CoreApplication lifecycle through the layer's shim; see
REM docs/WINRT-SHIM.md). Links kernelx.lib (one ERA import so the
REM loader installs the WinRT hooks) + vccorlib110.lib (the Xbox One
REM title activation path) + runtimeobject.lib (RoGetActivationFactory).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_winrt.cpp" /Fe"%OUT%\guest_winrt.exe" /Fo%OUT%\ /Fd%OUT%\guest_winrt.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" kernelx.lib vccorlib110.lib runtimeobject.lib
if errorlevel 1 exit /b 1

echo === 4. Verification harness ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\verify.cpp" /Fe"%OUT%\scarlett_verify.exe" /Fo%OUT%\ /Fd%OUT%\scarlett_verify.pdb /link "%OUT%\ScarlettCompat.lib"
if errorlevel 1 exit /b 1

echo === 5. Verify: guests through the layer ===
copy /Y "%OUT%\ScarlettCompat.dll" "%OUT%\ScarlettCompat.dll" >nul
cd /d "%OUT%"
"%OUT%\scarlett_verify.exe" "%OUT%\guest_minimal.exe" 60000
set RC1=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\ordinal\guest_ordinal.exe" 60000
set RC2=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\guest_proof.exe" 120000
set RC3=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\guest_era.exe" 120000
set RC4=%ERRORLEVEL%
REM RT mode (item 8): the software-rt guest runs once per mode. The
REM harness passes its environment through to the child, so
REM SCARLETT_RT_MODE reaches the guest. software = the default path,
REM off = RT calls are no-ops, hardware = falls back to software on
REM this tier-0 card (must NOT fail).
set SCARLETT_RT_MODE=software
"%OUT%\scarlett_verify.exe" "%OUT%\guest_software_rt.exe" 120000
set RC5=%ERRORLEVEL%
set SCARLETT_RT_MODE=off
"%OUT%\scarlett_verify.exe" "%OUT%\guest_software_rt.exe" 120000
set RC5b=%ERRORLEVEL%
set SCARLETT_RT_MODE=hardware
"%OUT%\scarlett_verify.exe" "%OUT%\guest_software_rt.exe" 120000
set RC5c=%ERRORLEVEL%
set SCARLETT_RT_MODE=
"%OUT%\scarlett_verify.exe" "%OUT%\guest_opt.exe" 180000
set RC6=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\guest_physics.exe" 120000
set RC7=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\guest_winrt.exe" 120000
set RC8=%ERRORLEVEL%
cd /d "%ROOT%"
if %RC1% neq 0 (echo guest_minimal FAILED & exit /b 1)
if %RC2% neq 0 (echo guest_ordinal FAILED & exit /b 1)
if %RC3% neq 0 (echo guest_proof FAILED & exit /b 1)
if %RC4% neq 0 (echo guest_era FAILED & exit /b 1)
if %RC5% neq 0 (echo guest_software_rt [software] FAILED & exit /b 1)
if %RC5b% neq 0 (echo guest_software_rt [off] FAILED & exit /b 1)
if %RC5c% neq 0 (echo guest_software_rt [hardware] FAILED & exit /b 1)
if %RC6% neq 0 (echo guest_opt FAILED & exit /b 1)
if %RC7% neq 0 (echo guest_physics FAILED & exit /b 1)
if %RC8% neq 0 (echo guest_winrt FAILED & exit /b 1)
echo === verify exit code: 0 ===
exit /b 0
