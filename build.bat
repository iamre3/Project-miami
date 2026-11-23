@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo [ERROR] Failed to initialize MSVC x64 developer environment.
    exit /b 1
)

if not exist "e:\ProjectMiami\out" mkdir "e:\ProjectMiami\out"

echo.
echo [1/3] Compiling ProjectMiami.exe (Modern PC Desktop Launcher)...
cl /nologo /O2 /W3 /std:c++17 /EHsc /utf-8 ^
   /I"e:\ProjectMiami\include" ^
   "e:\ProjectMiami\src\core\app.cpp" ^
   "e:\ProjectMiami\src\core\main.cpp" ^
   "e:\ProjectMiami\src\core\library.cpp" ^
   "e:\ProjectMiami\src\core\integration.cpp" ^
   "e:\ProjectMiami\src\config\config.cpp" ^
   "e:\ProjectMiami\src\install\installer.cpp" ^
   "e:\ProjectMiami\src\process\process.cpp" ^
   "e:\ProjectMiami\src\ui\renderer.cpp" ^
   /Fe:"e:\ProjectMiami\out\ProjectMiami.exe" ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib d2d1.lib dwrite.lib windowscodecs.lib dxgi.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile ProjectMiami.exe
    exit /b 1
)
echo [OK] ProjectMiami.exe generated at e:\ProjectMiami\out\ProjectMiami.exe

echo.
echo [2/3] Compiling test_miami.exe (Automated Verification Suite)...
cl /nologo /O2 /W3 /std:c++17 /EHsc /utf-8 ^
   /I"e:\ProjectMiami\include" ^
   "e:\ProjectMiami\tests\test_miami.cpp" ^
   "e:\ProjectMiami\src\core\app.cpp" ^
   "e:\ProjectMiami\src\core\library.cpp" ^
   "e:\ProjectMiami\src\core\integration.cpp" ^
   "e:\ProjectMiami\src\config\config.cpp" ^
   "e:\ProjectMiami\src\install\installer.cpp" ^
   "e:\ProjectMiami\src\process\process.cpp" ^
   /Fe:"e:\ProjectMiami\out\test_miami.exe" ^
   /link /SUBSYSTEM:CONSOLE ^
   user32.lib gdi32.lib shell32.lib ole32.lib dxgi.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Failed to compile test_miami.exe
    exit /b 1
)
echo [OK] test_miami.exe generated at e:\ProjectMiami\out\test_miami.exe

echo.
echo [3/3] Executing Automated Verification Suite...
"e:\ProjectMiami\out\test_miami.exe"
if errorlevel 1 (
    echo [ERROR] Verification suite failed!
    exit /b 1
)

echo.
echo ====================================================================
echo  PROJECT MIAMI BUILD AND VERIFICATION COMPLETED SUCCESSFULLY!
echo ====================================================================
exit /b 0
