# CLAUDE.md - QUA Player

## Project Overview

Monorepo for a high-performance ALSA audio player ecosystem targeting Linux.
Plays WAV via MMAP HW with zero-copy, zero-decoding during playback.
Follows Unix philosophy: small, focused components communicating via sockets/pipes.

## Sub-projects

| Directory | Component | Purpose |
|-----------|-----------|---------|
| `src/` | qua-player | Core audio player (ALSA MMAP, AVX2, PGO) |
| `1. daemon-socket/` | qua-socket | Socket daemon: IPC, cache, player selection |
| `2. launcher/` | qua-bare-launcher | Process launcher wrapper |
| `3. qua-convert/` | qua-convert | Audio format decoder/converter |
| `4. x11-hotkeys-daemon/` | qua-hotkeys | X11 global hotkey listener |
| `scripts/` | qua-play.sh | Shell orchestration |
| `hooks/` | prelaunch/teardown | User lifecycle hooks |

## Language & Toolchain

- **Language**: C (C23 standard, `-std=c23`)
- **Compiler**: GCC only. Never use or suggest C++.
- **Flags**: `-static -flto -fno-pie` as baseline. Use `-march=native` for new sub-projects.
- **Linking**: Static. No shared libraries in final binaries.
- **Target arch**: Zen4 primary, generic x86-64 acceptable.
- **Ask before creating new Makefiles** - developer will specify exact flags.

## Workflow

- **Explain before reading.** Before requesting to read any file, state why you need it.
  e.g. "I need to read qua-socket.c to understand the current accept loop before adding timeout logic."

## Build Rules

- **Never build.** The developer will compile and run. Do not run `make`, `gcc`, or any build command.
- Each sub-project has its own Makefile in its directory.
- PGO/BOLT builds use the `make-pgo*` scripts at root level.

## Coding Style

- **Linux kernel style**: tabs, K&R braces, ~80 columns.
- **snake_case** for functions and variables.
- **Match existing code** in each file for conventions (header guards, includes, etc.).

## Performance Philosophy

This is a performance-critical audio project. Design decisions should consider:

- **Register vs stack vs heap allocation** - prefer register/stack where possible.
- **Icache pressure** - keep hot paths small, avoid deep call chains.
- **Minimal overhead** - no unnecessary abstractions, indirection, or runtime checks.
- **Compiler intrinsics** are encouraged for performance-critical code.
- **Never auto-generate assembly** - developer writes asm by hand when needed.
- **Trust LTO + PGO** for general optimization; focus manual effort on hot loops.

## Error Handling

- Log to stderr and continue where possible.
- Don't over-handle errors or add excessive validation for internal code paths.

## Dependencies

- Minimal external dependencies. Small, well-known C libraries are acceptable.
- Core relies on: POSIX, Linux APIs, ALSA (statically linked custom build in `lib/`).
- Always ask before adding a new dependency.

## Testing

- No formal test framework. Manual testing by the developer.
- `test-audio/` contains sample WAV files at various bit-depth/sample-rate combos.
- Suggest new tests when making changes but don't run them.

## Directory Naming

- Existing sub-projects use numbered prefixes (`1. daemon-socket`, `2. launcher`, etc.).
- **Ask before creating new sub-project directories** - don't assume the naming scheme.

## Key Paths

- Socket: `/tmp/qua-socket.sock`
- Cache: `/dev/shm/qua-cache/`
- History: `~/.config/qua-player/history`
- Hooks: `hooks/prelaunch`, `hooks/teardown`
- Player binaries: `bin/qua-player-<bitdepth>-<samplerate>`
- Static ALSA: `lib/libasound.a` (gcc), `lib.musl/libasound.a` (musl)
