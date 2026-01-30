# qua-socket

A lightweight daemon for controlling qua-player via Unix domain sockets.

## Components

- `qua-socket` - The daemon (listens on `/tmp/qua-socket.sock`)
- `qua-send` - Shell client for sending commands

## Building

```sh
make
```

## Usage

Start the daemon:
```sh
./qua-socket
```

Control playback:
```sh
qua-send play song.flac    # Play a file
qua-send play              # Resume last played (from memory or history)
qua-send next              # Next track in directory
qua-send prev              # Previous track in directory
qua-send stop              # Stop playback
qua-send show              # Show current track info
qua-send history           # Pick from history with fzf
```

## Protocol

Commands are sent as null-terminated strings over the Unix socket:

```
action\0[data\0]
```

### Daemon Actions

| Action      | Data       | Response              |
|-------------|------------|-----------------------|
| play        | file path  | `Playing: filename`   |
| play        | (none)     | Resumes last played   |
| play-next   | (none)     | `Next: filename`      |
| play-prev   | (none)     | `Prev: filename`      |
| stop        | (none)     | `Stopped`             |
| show        | (none)     | Full file path        |

## Files

- `/tmp/qua-socket.sock` - Unix domain socket
- `/tmp/qua-socket-daemon.lock` - Lock file for single instance
- `~/.config/qua-player/history` - Play history (timestamp + path)

## Behavior

- **Single instance**: Only one daemon runs at a time (enforced via flock)
- **History**: Each played file is logged with timestamp
- **Startup**: Loads last valid file from history into memory
- **Next/Prev**: Scans directory for audio files, sorted alphabetically
- **Supported formats**: flac, mp3, m4a, opus, ogg, wv, wav, ape, aiff

## Dependencies

- `socat` - For the shell client
- `fzf` - For history selection
- `qua-play` - The actual player (spawned by daemon)
- `qua-info` - For showing track metadata
