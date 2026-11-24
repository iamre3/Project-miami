# File map — ScarlettCompat

ScarlettCompat is an open-source Xbox Series X (GDK) compatibility layer
for Windows. This file maps the repository's own source tree. **No game
code, assets, or package contents are in this repo.**

## Layout

- `build.bat` — builds the layer DLL, shim import libraries, and verify guests.
- `build_software_rt.bat` — builds the software ray-tracing test guest.
- `defs/` — module-definition files mapping Xbox DLL names to layer exports.
- `docs/` — architecture, integration, and status notes.
- `include/` — public layer headers (shims, wrappers, presentation).
- `src/` — layer implementation (PE loader, HLE registry, COM wrappers,
  WinRT shim, presentation, optimizations).
- `tests/` — verify guests (synthetic titles exercising the layer).

## Notes

- The layer presents Xbox GDK-style COM interfaces (D3D12, DirectStorage,
  GameInput, XAudio2, XMem) backed by desktop Windows APIs.
- See `docs/ARCHITECTURE.md` for the subsystem map and `docs/STATUS.md`
  for current progress.
