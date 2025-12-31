# QUA Player

A high-performance ALSA audio player built for wav playback on zen4, for USB DACs (snd_usb_audio) on Linux.

## General Features
- **Diverse file format support** plays FLAC,WavPack,ALAC,,mp3,opus,ogg, etc.
- **Pure 16-bit or 32-bit support** no extra padding for 16-bit in 32-bit.
- **All sample Rate support** 
- **Mono to stereo conversion** 
- **Multi-channel to stereo conversion** using ITU-R BS.775-3
- **User configurable** can force specific sample-rate or bitdepth in pre-processing script

## Technical Features
- **Zero-decoding, Zero-reading during playback** File fulled decoded, and loaded into program memeory prior to start of playback
- **Zero-copy (MMAP) audio playback** using ALSA HW MMAP
- **Huge Page Utilization** to reduce TLB overhead when transfering to DMA buffer.
- **CPU core locking** to minimize context switching and cache misses
- **Non-Temporal AVX stream load and store** to minimize cache pollution
- **Hand-rolled ASM for x86-64 AVX2** for memcopy
- **PGO + Bolt Optimization** to reduce icache misses and branch predicition accuracy
- **Modified asoundlib** to minimize call graph, eliminated unnesscary checks.
- **Maxmimum Buffer Utilization for individual sample rate and bit depth combination**
- **Numerous compilier hints, tricks, and hacks in source code**

## Non-Features
This is a minimal, focused audio player. It does **not** include:
- GUI or interactive controls during playback
- Playlists or queue management
- Seeking
- External control interfaces (no remote control, web interface, etc.)

## General Behavior
The player loads a file into memory and plays it from start to finish, then exits. If the player is called again during playback, it will terminate the previous instance and start playing the new file.

## Why it is unlikely for other players to have better sound quality
In general software distribution, software needs to maintain general compatibility serveral different hardware architecture and kernel configuration, preventing highly optimized builds. For example, there are no assumptions that there are huge Page support in the kernel orconsideration of uops costs of specific SMID instructions for the CPU (for example, on zen4, AVX-512 is double pumped). There are generally no profile guided optimization or post-link optimization done to the distributed packages. 

In regard to audio players, the focus is generally on UI, compatibility, low-latency playback, which will to a various degree add some CPU work during playback. [unfinished]


# Format Support
The player handles only 32-bit WAV files, but the setup script converts various formats:
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

## Installation

**Clone and build:**
   ```bash
   git clone <repository-url>
   cd qua_play
   make
   ```

**Install:**
   ```bash
   make install
   ```

## Usage

### Basic Playback (with conversion)
```bash
qua_handler audio_file.flac
qua_handler audio_file.wav hw:1,0  # Specify audio device
```

### Direct Player Usage
```bash
# For pre-converted 32-bit WAV files
qua_player_32 audio_file.wav hw:0,0
```

### Configuration

Edit `qua_setup` and to customize:

```bash
CORES="2,3"                    # CPU cores for audio thread
VALID_SAMPLE_RATES="44100 48000 88200 96000 176400 192000 352800 384000"
FORCE_SAMPLE_RATE=false        # Or set specific rate
FALLBACK_SAMPLE_RATE=96000
VALID_BIT_DEPTH="16 32"
FORCE_BIT_DEPTH=32
```

## How It Works

### Audio Pipeline
1. **Format Detection** - Analyzes input file format and properties
2. **Conversion** - Converts to 32-bit WAV via format-specific decoders
3. **Processing** - Applies sample rate conversion and bit-padding with SoX
4. **Playback** - Real-time playback with optimized memory access patterns

### Performance Optimizations
- **Zero-copy**: Direct memory mapping to DMA buffers
- **Branch Prediction**: Optimized control flow for better performance

## Technical Details

### Memory Management
- Memory locking (`mlockall`) prevents page faults during playback

### Performance Features
- CPU affinity binding via `taskset`
- Process isolation and priority scheduling
- Minimal system call overhead in audio thread
- Some hand-optimized AVX2 memcpy for zen4

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
