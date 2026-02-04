# qua-socket Protocol Specification

## Socket

| Property | Value |
|----------|-------|
| Type | Unix domain socket (SOCK_STREAM) |
| Path | `/tmp/qua-socket.sock` |
| Buffer | 4096 bytes |

## Message Format

```
<action>\0[<data>\0...]
```

Null-terminated fields. First field is action, rest is action-specific.

## Actions

### play

**Request**: `play\0` or `play\0<filepath>\0`

**Response**: `Playing: <basename>\n` or `Nothing playable\n`

**Logic**:
```
if filepath provided:
    use filepath
else if last_played exists and file accessible:
    use last_played
else:
    scan history for most recent valid file

if found:
    spawn qua-play
    log to history
    update last_played
    respond "Playing: basename"
else:
    respond "Nothing playable"
```

### play-next / play-prev

**Request**: `play-next\0` or `play-prev\0`

**Response**: `Next: <basename>\n` or `Prev: <basename>\n`

**Logic**:
```
dir = dirname(last_played)
files = scandir(dir, filter=audio, sort=alpha)
idx = find(files, last_played)
next_idx = (idx + offset) % len(files)  # +1 or -1
play(files[next_idx])
```

Wraps around at boundaries.

### stop

**Request**: `stop\0`

**Response**: `Stopped\n`

**Logic**:
```
kill_player()
run_hook_async(teardown)
```

Uses regex to match all player variants. Runs teardown hook async to restore environment.

### show

**Request**: `show\0`

**Response**: `<full path>` (no newline)

Returns `last_played` or empty string.

## Audio Detection

Extension-based, fast first-char dispatch:

| Char | Extensions |
|------|------------|
| a | ape, aiff, aif |
| f | flac |
| m | mp3, m4a |
| o | opus, ogg |
| w | wv, wav |

## Cache Management

The daemon handles all cache operations for converted audio files.

**Cache Directory**: `/dev/shm/qua-cache/`

**Cache Path Format**: `qua-<inode>-<mtime>.wav`

**Max Size**: 2GB (LRU eviction when exceeded)

### Cache Flow (Play)

```
┌─────────────────────────────────────┐
│  cache_generate_path(audio_file)    │
│  → /dev/shm/qua-cache/qua-XXX.wav   │
└─────────────────┬───────────────────┘
                  │
                  ▼
          ┌───────────────┐
          │  Cache hit?   │
          └───────┬───────┘
                  │
       ┌──────────┴──────────┐
       │ YES                 │ NO
       ▼                     ▼
┌─────────────┐    ┌─────────────────────┐
│ Skip decode │    │ cache_manage_size() │
│             │    │ run_convert()       │
└──────┬──────┘    │ (spawns qua-convert)│
       │           └──────────┬──────────┘
       │                      │
       └──────────┬───────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│  select_player(cache_path)          │
│  → qua-player-<bd>-<sr>[.pgoXXX]    │
└─────────────────┬───────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│  launch_player(player, cache_path)  │
└─────────────────────────────────────┘
```

### Prefetching

After playing a track, the daemon prefetches the next track in background:

```c
prefetch_next(current_path)
    │
    ├─ get_next(current_path, +1, next_path)
    ├─ cache_generate_path(next_path)
    ├─ if cache_exists() → skip
    └─ double-fork + exec(qua-convert)  // fire and forget
```

### Player Selection

Based on WAV header (bit-depth + sample-rate):

| Bit Depth | Sample Rate | Player Binary |
|-----------|-------------|---------------|
| 16 | 44100 | qua-player-16-44100 |
| 16 | 48000 | qua-player-16-48000 |
| 32 | 44100 | qua-player-32-44100 |
| 32 | 96000 | qua-player-32-96000 |
| ... | ... | ... |

Tries PGO-optimized binary first (`.pgoXXXX` suffix), falls back to generic.

## History File

**Path**: `$XDG_CONFIG_HOME/qua-player/history` or `~/.config/qua-player/history`

**Format**:
```
<unix-timestamp> /full/path/to/file
```

**Operations**:
- Append on each play
- Read on startup (populate last_played)
- Read as fallback when no last_played

## Hooks

User-configurable shell scripts executed at specific lifecycle points.

### Hook Files

| Hook | Execution | Purpose |
|------|-----------|---------|
| `prelaunch` | Before play, blocking | Setup before playback (stop services, etc.) |
| `teardown` | After stop, async | Restore environment (restart services, etc.) |

**Path**: `/home/free2/code/musl2gcc/hooks/` (hardcoded)

**Note**: For async tasks within a hook, background them with `&` in the shell script.

### Play Flow (Fork-Join Pattern)

```
┌─────────────────────────────────────┐
│  cache_generate_path(audio_file)    │
│  cache_exists() → hit or miss       │
└─────────────────┬───────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│         FORK (parallel)             │
├─────────────────────────────────────┤
│  kill_player_async()                │──┐
│                                     │  ├─ run in parallel
│  run_hook(prelaunch)                │──┘
└─────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│         JOIN (barrier)              │
├─────────────────────────────────────┤
│  kill_player_wait()                 │
└─────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│     CONVERT (if cache miss)         │
├─────────────────────────────────────┤
│  cache_manage_size()                │
│  run_convert(input, cache_path)     │
└─────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│         SELECT + SPAWN              │
├─────────────────────────────────────┤
│  select_player(cache_path)          │
│  launch_player(player, cache_path)  │
└─────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│         PREFETCH                    │
├─────────────────────────────────────┤
│  prefetch_next(audio_file)          │
└─────────────────────────────────────┘
```

### Stop Flow

```
kill_player()                # Kill player (blocking)
    ↓
run_hook_async(teardown)     # Restore environment (fire and forget)
```

### Key Behavior

- **Song transitions**: No teardown, kill runs parallel with prelaunch hook
- **Explicit stop**: Teardown runs (restores picom, pipewire)
- **Hook arguments**: Audio file path passed as `$1`
- **Exit codes**: Ignored (hooks always continue)

## Process Management

### Spawning Players

Uses double-fork pattern so init reaps the player process:

```c
launch_player(player_path, wav_path):
    fork()
    └─ fork()  // grandchild
       └─ exec(qua-bare-launcher, "4", player_path, wav_path, "hw:0,0")
       exit(0)  // intermediate exits
    waitpid()  // reap intermediate (instant)
```

The `qua-bare-launcher` handles the final process setup (nice level, stdout redirect).

### Spawning Converter

```c
run_convert(input, output):
    posix_spawnp(&pid, "qua-convert", NULL, NULL, args, environ)
    waitpid(pid, &status, 0)  // blocking wait
    verify output file exists
```

### Signal Handling

| Signal | Handler |
|--------|---------|
| SIGPIPE | Ignored |

No SIGCHLD handler - uses explicit `waitpid()` for all children. Fire-and-forget children (player, prefetch) use double-fork so init reaps them.

### Single Instance

```c
fd = open("/tmp/qua-socket-daemon.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
flock(fd, LOCK_EX | LOCK_NB);  // fails if already running
```

## Startup Sequence

1. `init_paths()` - resolve config dir and hook paths
2. `cache_init()` - ensure cache directory exists
3. `find_playable_from_history()` - populate last_played
4. Acquire lock file
5. `unlink()` stale socket
6. `socket()` + `bind()` + `listen()`
7. Setup spawn attributes
8. Setup signal handlers (SIGPIPE ignored)
9. Accept loop

## Request Lifecycle

```
accept(client_fd)
    ↓
read(client_fd, buf, 4096)
    ↓
parse: action = buf, data = buf + strlen(action) + 1
    ↓
dispatch(action, data, client_fd)
    ↓
close(client_fd)
```

Single-threaded, blocking. One request at a time.

## Error Behavior

| Condition | Behavior |
|-----------|----------|
| Unknown action | No response, connection closed |
| play-next with no last_played | No response |
| File not found | Falls back to history |
| History empty | "Nothing playable" |
| Client disconnects | SIGPIPE ignored, no crash |
| Second daemon instance | Exits with error |
