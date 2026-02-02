# qua-convert Control Flow

## Overview

`qua-convert` is a conversion-only tool. It converts audio files to WAV format and reports the appropriate player binary. It does **not** launch the player—that responsibility belongs to the upstream daemon.

## Output Format

On success, `qua-convert` prints two lines to stdout:
```
<player-binary-path>
<wav-file-path>
```

Example:
```
/usr/local/bin/qua-player-32-44100.pgo5992
/home/user/.cache/qua/abc123.wav
```

## Flow

```
┌─────────────────────────────────────────┐
│           qua-convert <file>            │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│         Resolve input file path         │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│      Generate cache file path           │
└─────────────────┬───────────────────────┘
                  │
                  ▼
          ┌───────┴───────┐
          │  Cache hit?   │
          └───────┬───────┘
                  │
       ┌──────────┴──────────┐
       │ YES                 │ NO
       ▼                     ▼
┌─────────────┐    ┌─────────────────────┐
│ notify-send │    │ Manage cache size   │
│ (cache hit) │    │ Decode audio        │
└──────┬──────┘    │ Post-process if     │
       │           │ needed (resample,   │
       │           │ bit-depth, channels)│
       │           └──────────┬──────────┘
       │                      │
       └──────────┬───────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│   Select player binary based on WAV     │
│   specs (bit-depth, sample-rate)        │
│   Try PGO binary first, fallback to     │
│   generic                               │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│   Print to stdout:                      │
│     Line 1: player binary path          │
│     Line 2: WAV file path               │
└─────────────────────────────────────────┘
```

## Daemon Integration

The daemon (socket-based) orchestrates playback:

```
client sends play request via socket (fire and forget)
    │
    ▼
daemon spawns qua-convert, waits for exit
    │
    ▼
qua-convert does work, prints result to stdout, exits
    │
    ▼
daemon reads stdout → gets player path + wav file
    │
    ▼
daemon launches player (vfork/exec)
```

This keeps qua-convert as a pure conversion tool with no socket/daemon knowledge. The daemon handles all orchestration.

## Exit Codes

- `0`: Success, player and WAV path printed to stdout
- `1`: Error (file not found, conversion failed, no player binary found)

## Errors

All errors are printed to stderr. Stdout contains only the two-line output on success.
