# QUA Player

A high-performance audio player built with ALSA and optimized for playback of WAV files. 

## Features

- **Zero-copy audio playback** using ALSA mmap interface
- **Pre-calculated memory layout** for minimal runtime overhead  
- **Cache-aligned data structures** for optimal CPU performance
- **Huge page memory allocation** with automatic fallback
- **Memory locking** to prevent page faults
- **Multi-format audio support** via conversion pipeline
- **CPU core affinity** to minimize context switching and cache misses

## Non-Features

This is a minimal, focused audio player. It does **not** include:
- Playlists or queue management
- Seeking or scrubbing through audio
- External control interfaces (no remote control, web interface, etc.)
- GUI or interactive controls during playback
- Pause/resume functionality

The player loads a file into memory and plays it from start to finish, then exits. If the player is called again during playback, it will terminate the previous instance and start playing the new file.

The player natively handles 32-bit WAV files, but the setup script converts various formats:
- FLAC
- WavPack (`.wv`)
- Monkey's Audio (`.ape`) 
- MP3
- M4A/MP4
- Opus
- OGG
- AIFF

## Requirements

### System Dependencies
```bash
# Ubuntu/Debian
sudo apt install libasound2-dev sox flac wavpack monkeys-audio lame ffmpeg

# Arch Linux  
sudo pacman -S alsa-lib sox flac wavpack mac lame ffmpeg
```

### Audio Hardware
- ALSA-compatible audio interface
- Support for 32-bit signed integer format
- Configurable sample rates (44.1kHz - 384kHz)

## Installation

1. **Clone and build:**
   ```bash
   git clone <repository-url>
   cd qua_play
   make
   ```

2. **Install:**
   ```bash
   make install
   ```

## Usage

### Basic Playback
```bash
qua_handler audio_file.flac
qua_handler audio_file.wav hw:1,0  # Specify audio device
```

### Configuration

Edit `qua_setup` to customize:

```bash
CORES="2,3"                    # CPU cores for audio thread
VALID_SAMPLE_RATES="44100 48000 88200 96000 176400 192000 352800 384000"
FORCE_SAMPLE_RATE=false        # Or set specific rate
FALLBACK_SAMPLE_RATE=96000
VALID_BIT_DEPTH="16 32"
FORCE_BIT_DEPTH=32
```

### Direct Player Usage
```bash
# For pre-converted 32-bit WAV files
qua_player_32 audio_file.wav hw:0,0
```

## How It Works

### Audio Pipeline
1. **Format Detection** - Analyzes input file format and properties
2. **Conversion** - Converts to 32-bit WAV via format-specific decoders
3. **Processing** - Applies sample rate conversion and padding with SoX
4. **Playback** - Real-time playback with optimized memory access patterns

### Performance Optimizations

- **Pre-calculated Pointers**: All memory addresses computed during initialization
- **Cache Alignment**: Hot data structures aligned to 64-byte cache lines  
- **Memory Layout**: Separate hot/cold data paths to minimize cache misses
- **Zero-copy**: Direct memory mapping to audio hardware buffers
- **Branch Prediction**: Optimized control flow for better performance

## Technical Details

### Memory Management
- Attempts 2MB huge pages, falls back to 4KB pages
- Memory locking (`mlockall`) prevents page faults during playback
- Ring buffer with precalculated wraparound handling

### Performance Features
- CPU affinity binding via `taskset`
- Process isolation and priority scheduling
- Minimal system call overhead in audio thread

### Debug Mode
Compile with debug information:
```bash
make DEBUG=1
```

## Troubleshooting

### Common Issues
- **Permission denied**: Add user to `audio` group
- **Device busy**: Check if other applications are using audio device
- **Underruns**: Increase buffer size or check CPU affinity settings

### Audio Device Detection
```bash
# List available ALSA devices
aplay -l

# Test device access
aplay -D hw:0,0 /usr/share/sounds/alsa/Front_Left.wav
```
