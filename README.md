# PROJECT MIAMI
### Standalone PC Game Launcher for Grand Theft Auto VI

**Project Miami** is a dedicated Windows PC launcher engineered to manage configuration, presentation, and execution of Grand Theft Auto VI on PC. It delivers a modern desktop interface inspired by contemporary PC gaming releases, free from console dashboard layouts, emulator menus, fake statistics, or developer clutter.

---

## How It Works

Project Miami acts as the central bridge between the game installation, underlying compatibility layers, and your PC hardware:

1. **System & Asset Validation:**  
   During initial setup, the launcher inspects your selected game directory to verify that the required master executable and game data packages are present and intact. It will not allow launching unless authentic game files are detected.

2. **Multi-Layer Runtime Coordination:**  
   The launcher coordinates three specialized subsystems behind the scenes:
   - **Game Core Compatibility Layer:** Binds system runtime interfaces, manages memory pool allocations, and orchestrates Direct3D 12 device creation.
   - **Static Recompilation Framework:** Translates console GPU shaders into native desktop SPIR-V and Direct3D 12 bytecode, deswizzles textures, and unpacks multi-channel audio.
   - **Architecture Layer:** Provides system ordinal definitions and hardware abstraction shims for native Windows execution.

3. **Direct2D Hardware-Accelerated Presentation:**  
   The interface is rendered natively using Direct2D and DirectWrite at 60 FPS with responsive resizing, custom window controls, and atmospheric visual styling.

4. **Lifecycle & Process Management:**  
   Launches the game as a monitored child process with live status polling, heartbeat tracking, and crash-guard isolation.

---

## Features

- **Cinematic Home Screen:** High-impact visual presentation centered around Grand Theft Auto VI with an interactive **Play Now** action.
- **5-Step Setup Wizard:**
  - *Welcome:* Introduction to the launcher.
  - *Folder Selection:* Native folder picker to locate game files.
  - *Verification:* Genuine detection of required files.
  - *Configuration:* Safe default preferences.
  - *Completion:* Direct entry into the launcher.
- **Dedicated Settings:** Two-column layout with category selection on the left and preferences on the right (Display, Performance, Audio, Input, and System).
- **Game Library:** Clean grid layout of installed titles with artwork previews and selection support.
- **Persistent Preferences:** User settings and display configurations are automatically saved and restored across sessions.

---

## Instructions

### Building the Launcher
1. Open a Command Prompt or PowerShell terminal.
2. Run the build script:
   ```cmd
   build.bat
   ```
3. The build script automatically locates the MSVC compiler, compiles the launcher executable, and runs the automated test suite.

### Running the Launcher
After building, run:
```cmd
ProjectMiami.exe
```

### First-Time Configuration
1. On first launch, follow the guided setup wizard.
2. Browse to and select your Grand Theft Auto VI installation folder.
3. Once the launcher confirms the required files, complete setup to enter the main dashboard.
4. Click **Play Now** to start the game, or customize your resolution and controls in **Settings**.

---

## License & Attribution

Project Miami is an independent launcher project and does not distribute copyrighted game assets, executables, or encryption keys. Users provide their own legally obtained game installation.
