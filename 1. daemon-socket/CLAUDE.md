# CLAUDE.md - qua-socket (daemon-socket)

## Role

Central hub of the qua-player ecosystem. Handles all IPC, cache management,
player selection, and playback lifecycle.

## Status

Actively being developed. Expect frequent changes.

## Key Files

- `qua-socket.c` - Main daemon: accept loop, command dispatch, player lifecycle
- `qua-cache.c/h` - Cache path generation, LRU eviction, size management
- `qua-player-selector.c/h` - WAV header parsing, player binary selection
- `qua-send` - Shell client (uses socat)

## Architecture

- Single-threaded blocking accept loop (one request at a time, by design).
- Uses fork/double-fork for concurrency (player spawning, prefetch).
- No threads. No async I/O.

## Protocol

See `PROTOCOL.md` for full spec. Commands are null-terminated strings over
Unix domain socket at `/tmp/qua-socket.sock`.

## Conversion

Spawns `qua-convert` via `posix_spawnp` and blocks (`waitpid`) until
conversion completes before proceeding to playback.
