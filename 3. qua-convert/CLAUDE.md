# CLAUDE.md - qua-convert

## Role

Audio format decoder. Converts any supported input format (FLAC, WavPack,
APE, MP3, M4A, Opus, OGG, AIFF) to WAV for playback by qua-player.

## Status

Functional.

## Behavior

Called by the daemon (qua-socket) as a blocking subprocess. The daemon
waits for conversion to complete before proceeding to playback.

## Key Files

- `qua-convert.c` - Main entry, format dispatch
- `qua-decode.c/h` - Format-specific decoding logic
- `qua-config.c/h` - Configuration
- `qua-post-processing.c/h` - Post-decode processing (sample rate, bit depth)
