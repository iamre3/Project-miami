@echo off
REM Software ray tracing — standalone build + host test.
REM Builds tests/software_rt_test.cpp + src/software_rt.cpp as a normal
REM Win32 exe (NOT an Xbox guest) and runs it on the real GPU. This proves
REM the backend itself before the layer guest version runs through the
REM harness. Output goes to out\srt\ so it never touches the phase 5
REM out\ binaries. RUN THIS IN AN INTERACTIVE SESSION (session 1) or a
REM foreground SSH session — it creates a real D3D12 device but no
REM swapchain, so no compositor is needed. Nothing here touches
REM D:\game\game.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%out\srt
if not exist "%OUT%" mkdir "%OUT%"

echo === software_rt host test ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\software_rt_test.cpp" "%ROOT%src\software_rt.cpp" /Fe"%OUT%\software_rt_test.exe" /Fo%OUT%\ /Fd%OUT%\software_rt_test.pdb /link d3d12.lib d3dcompiler.lib dxgi.lib
if errorlevel 1 exit /b 1

cd /d "%OUT%"
"%OUT%\software_rt_test.exe"
set RC=%ERRORLEVEL%
cd /d "%ROOT%"
if %RC% neq 0 (echo software_rt host test FAILED & exit /b 1)
echo === software_rt host test PASS ===
exit /b 0
