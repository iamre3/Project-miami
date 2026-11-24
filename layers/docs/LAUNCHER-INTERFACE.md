# Launcher interface — driving the layer from an external launcher

For R33's launcher GUI (Seth, 2026-11-23). No launcher code lives in
this repo; this note is the contract so an external launcher can drive
the layer instead of the current script / scheduled-task route.

## The entry point

One executable, one command line. The launcher runs it directly (no
scheduled task, no .bat wrapper needed):

```
scarlett_verify.exe "<full path to game exe>" [timeout_ms]
```

- **Arg 1 — exe path (required):** the full path to the title exe,
  e.g. `D:\game\game\game_scarlett_master.exe`. It MUST be an
  absolute path. A relative path fails with CreateProcess error 267,
  because the harness derives the child's working directory from the
  exe path (see below).
- **Arg 2 — timeout_ms (optional, default 60000):** how long the
  parent waits for the child before declaring a timeout. Real-exe
  boots use `300000` (5 minutes). A timeout is a hang, not a pass.
- **Do NOT call `--run` yourself.** `scarlett_verify.exe --run
  "<exe>"` is the internal child mode: the parent spawns itself as
  a child with `--run` so the guest runs in a separate process that
  can crash without taking the parent down. The launcher always
  calls the parent form (no `--run`).

## Game data directory

There is no separate "game data dir" argument. The harness sets the
child process's working directory to the **directory containing the
exe**, and the title finds its data relative to that. So the exe must
sit in the game data folder:

```
<game data dir>\game_scarlett_master.exe   (e.g. D:\game\game\...)
```

If the data moves (e.g. D: → F:), point the launcher at the new exe
path and the data dir follows automatically. The layer never writes
into the game data folder; all loader-side patches happen in the
mapped image only, and all logs/dumps go elsewhere (below).

## Presentation — what the user sees

Two hard rules (Seth, 2026-11-23), implemented in
`include/present.h` / `src/present.cpp`:

- **720p WINDOWED, never fullscreen.** When the title's presentation
  path creates its window/swapchain, it is 1280x720 windowed
  (`WS_OVERLAPPEDWINDOW`, centered). The launcher does not need to
  size anything; the layer enforces it. There is no fullscreen flag
  to set or clear.
- **BLACK until the title loads.** The screen stays BLACK unless the
  game itself is trying to load. Default presentation is a black
  clear; only once the title has actually submitted a real frame does
  anything else show. No test pattern, no red triangle, no
  placeholder in the real-exe path. (The red triangle lives in the
  synthetic proof guest only — it is a test artifact, never shown
  for the real exe.)

There are no presentation flags on the command line: black-until-
loading and 720p-windowed are the layer's defaults, not options.

## Reading the result

The harness writes its log to **stdout** — redirect it to a file:

```
scarlett_verify.exe "D:\game\game\game_scarlett_master.exe" 300000 > boot.log 2>&1
```

Key lines, in order:

- `loader PASS` / `587/587 imports, 0 failed` — the exe mapped and
  its imports resolved. If this fails, the exe path or the layer
  DLLs are wrong.
- `GameInputCreate() -> S_OK (STUB)`, `DStorageGetFactory(...) ->
  S_OK (STUB, ...)` — the platform-service stubs answered.
- `guest crashed in entry: exception 0x...` — the guest faulted;
  the exception code and fault address follow. The child exit code
  (below) tells you how it ended.
- `--- verdict: ALL PASS ---` — only for the synthetic verify
  guests, not for a real-exe boot. A real-exe boot has no PASS
  verdict; success is "the game reached its next milestone" (entry
  point, device creation, first frame, menus), read from the log.

**Exit codes** (the parent's exit code):

| Code | Meaning |
|---|---|
| `0` | Clean exit (synthetic guest passed, or the title exited on its own). |
| `0xc0000005` | Access violation in the guest (the usual early-init crash). |
| `0x80000003` / `0xe0000003` | Breakpoint / int3 — a deliberate assert trap in the guest. |
| C++ exception | Unhandled C++ exception in the guest. |
| `0xe0000004` | Timeout (child hung past `timeout_ms`). |
| other non-zero | Read the log's last lines; the fault address is in them. |

**Crash dumps:** on a guest crash the harness writes a full-memory
dump to `F:\ScarlettCompat\out\scarlett_crash.dmp` (next to the
harness, never into the game folder; ~5 GB while the big XMem arenas
are committed). Windows Error Reporting may also write
`%LOCALAPPDATA%\CrashDumps\scarlett_verify.exe.<pid>.dmp`. The
launcher does not need to collect these; they are for diagnosis.

## Environment variables (optional)

| Variable | Values | Default |
|---|---|---|
| `SCARLETT_RT_MODE` | `off` / `software` / `hardware` | `software` (compute-shader ray tracer; the RX 5700 XT has no RT hardware) |
| `SCARLETT_PHYSICS_HZ` | number in Hz | `60` (fixed-timestep physics rate) |

Set them in the launcher's child environment if you want to change
them; unset is the default in both cases.

## Current state (2026-11-23)

The real exe still dies in static/global init — every boot so far
crashes before main, before any window, device, or present. That is
expected right now: the launcher will see a crash (usually
`0xc0000005`) and a log ending in static init. That is not a launcher
bug. As the push lands each milestone (init → entry → device →
first frame → menus), the same command line will get further; the
interface does not change.

## What the launcher does NOT need to do

- No scheduled tasks. The old `ScarlettRealExeBoot<N>` tasks were
  only needed because the build/boot runs had to be in the
  interactive session (session 1) — a launcher the user runs
  interactively is already in session 1.
- No .bat wrappers. The command line above is the whole interface.
- No window sizing, no fullscreen handling, no test-pattern
  suppression. The layer owns presentation (black + 720p windowed).
- No game-data copying or exe placement. The exe path you pass IS
  the data dir.
- Never write into the game data folder, and never modify the exe
  on disk. All patches are loader-side, in the mapped image only.
