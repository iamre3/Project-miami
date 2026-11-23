# PROJECT MIAMI
### Standalone PC Game Launcher for Grand Theft Auto VI
**Platform:** Windows 10/11 x64  
**Design Direction:** Modern PC Gaming Launcher (Obsidian, Miami Cyan `#00f2fe`, Sunset Pink `#e8507a`)  
**Graphics Presentation:** Direct2D, DirectWrite, WIC, DXGI (Hardware-accelerated 60 FPS)

---

## 1. Product Overview

**Project Miami** is an independent, production-quality Windows PC game launcher designed from scratch. It delivers a modern, cinematic interface that prioritizes the game itself, free from console dashboard layouts, emulator menus, fake statistics, or developer clutter.

### Core Architecture & Multi-Layer System
Project Miami coordinates three specialized compatibility and recompilation layers:
1. **XENITH Core Subsystems (`e:\XENITH`):** PE32+ loader, GDK Game Core runtime shims (`xgameruntime.dll`, `xmem.dll`), memory pool management (`MEM_GRAPHICS`), and Direct3D 12 device creation.
2. **Leonida Recompilation Framework (`e:\XENITH\references\Leonida`):** RDNA2-to-SPIR-V shader translation toolchain, AWC audio extractors, BCPack texture decompression, and static recompilation engine.
3. **GTA6-COMPAT-LAYER (`e:\XENITH\references\GTA6-COMPAT-LAYER`):** Xbox Series Scarlett architecture headers (`scarlett.h`), module DEF ordinal exports (`d3d12_xs.def`, `gameinput.def`, etc.), and HLE registry bindings.
- **Cinematic Home Screen:** High-impact background, game title, installation status badge, metadata panel, and a primary **"Play Now"** action.
- **Streamlined Setup Wizard (5 Steps):**
  1. *Welcome:* Clean brand introduction.
  2. *Folder Selection:* Native Windows folder picker (`SHBrowseForFolder`).
  3. *Validation:* Honest inspection of directory existence and executable detection.
  4. *Configuration:* Automatic generation of safe default preferences.
  5. *Completion:* Confirmation and seamless transition to the main launcher.
- **Genuine Settings System:** Two-column desktop layout (Sidebar categories on left, preferences on right) covering Display, Performance (Target FPS, VSync, Frame Pacing), Audio, Input (Controller / Keyboard-Mouse), and System.
- **Integrated Game Library:** Clean grid layout of installed titles with artwork previews and selection support.
- **Modular Compatibility Adapter:** Decoupled runtime bridge interface (`ICompatibilityAdapter`) allowing runtime adapters like Leonida to be integrated without coupling to UI code.
- **Process Manager:** Non-blocking child process launcher with live PID tracking, uptime recording, and error reporting.

---

## 2. Directory Structure

```
e:\ProjectMiami\
├── .git\                    # Local Git repository (Author: iamre3)
├── .gitignore               # Excludes binaries, build artifacts, and logs
├── assets\
│   └── images\              # High-resolution launcher backgrounds & branding
├── include\
│   └── miami\
│       ├── app.h            # Main application coordinator
│       ├── config.h         # INI configuration serializer & logger
│       ├── installer.h      # Installation directory validator
│       ├── integration.h    # Modular compatibility runtime adapter interface
│       ├── library.h        # Game library model and catalog
│       ├── process.h        # Windows process lifecycle manager
│       └── renderer.h       # Direct2D/DirectWrite rendering system
├── out\
│   ├── ProjectMiami.exe     # Standalone PC launcher executable
│   └── test_miami.exe       # Automated 24-assertion test suite
├── src\
│   ├── config\config.cpp
│   ├── core\app.cpp
│   ├── core\integration.cpp
│   ├── core\library.cpp
│   ├── core\main.cpp        # WinMain entry point & message loop
│   ├── install\installer.cpp
│   ├── process\process.cpp
│   └── ui\renderer.cpp      # Direct2D PC interface implementation
├── tests\
│   └── test_miami.cpp       # Unit & integration test runner
├── build.bat                # One-click MSVC x64 build script
└── README.md
```

---

## 3. How to Build & Run

### Prerequisites
- Windows 10/11 x64
- MSVC x64 (Visual Studio 2022 or Build Tools)
- Windows SDK (`d2d1.lib`, `dwrite.lib`, `windowscodecs.lib`, `dxgi.lib`)

### Building
Run `build.bat` in Command Prompt or PowerShell:
```cmd
cmd.exe /c "e:\ProjectMiami\build.bat"
```
This compiles `ProjectMiami.exe`, builds `test_miami.exe`, and runs the verification suite.

### Launching
Run the generated executable:
```cmd
e:\ProjectMiami\out\ProjectMiami.exe
```

---

## 4. Automated Verification Suite

The test suite (`out\test_miami.exe`) executes 24 honest assertions:
1. **Configuration Persistence & Recovery:** INI serialization, resolution persistence, and graceful recovery from corrupt syntax.
2. **Directory Validation:** Non-existent folder rejection, executable inspection, and file size checks.
3. **Library Management:** Catalog queries, active game selection, and invalid ID rejection.
4. **Process Manager:** Launch error detection on missing executables, process creation, state polling, and clean termination.
5. **Hardware Inspection:** OS version detection, CPU model string, RAM capacity, and GPU identification.
6. **Integration Interface:** Adapter registration, name verification, and availability reporting.

---

## 5. Persistent Data Locations

- **Configuration File:** `%APPDATA%\ProjectMiami\miami_config.ini`
- **Application Logs:** `%APPDATA%\ProjectMiami\miami_launcher.log`
