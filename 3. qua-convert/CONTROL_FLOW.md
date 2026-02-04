# qua-convert Control Flow

## Overview

`qua-convert` is a pure conversion tool. It converts audio files to WAV format. It does **not** manage caching, select players, or launch playback—those responsibilities belong to the socket daemon (`qua-socket`).

## Usage

```
qua-convert <input-audio-file> <output-wav-path>
```

## Flow

```
┌─────────────────────────────────────────┐
│  qua-convert <input> <output>           │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│         Resolve input file path         │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│   Decode audio to output WAV file       │
│   (FLAC, WavPack, etc.)                 │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│   Post-process if needed:               │
│   - Bit depth conversion (16/24→32)     │
│   - Sample rate conversion              │
│   - Channel mixing (mono→stereo)        │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│   Exit 0 (output WAV exists)            │
└─────────────────────────────────────────┘
```

## Exit Codes

- `0`: Success, output WAV file created
- `1`: Error (file not found, conversion failed)

## Errors

All errors are printed to stderr.
