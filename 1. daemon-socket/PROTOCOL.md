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

## History File

**Path**: `$XDG_CONFIG_HOME/qua-player/history` or `~/.config/qua-player/history`

**Format**:
```
YYYY-MM-DD HH:MM:SS /full/path/to/file
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
│         FORK (parallel)             │
├─────────────────────────────────────┤
│  kill_player_async()                │──┐
│                                     │  ├─ run in parallel
│  run_hook(prelaunch)                │──┘
└─────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│         JOIN (barrier)              │
├─────────────────────────────────────┤
│  kill_player_wait()                 │
└─────────────────────────────────────┘
                 ↓
┌─────────────────────────────────────┐
│         SPAWN                       │
├─────────────────────────────────────┤
│  posix_spawn(qua-play)              │
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

```c
posix_spawnattr_t attr;
flags = POSIX_SPAWN_SETSID        // new session
      | POSIX_SPAWN_SETSIGDEF     // reset signals
      | POSIX_SPAWN_SETSIGMASK;   // clear mask

// stdout -> /dev/null
posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

posix_spawnp(&pid, "qua-play", &fa, &attr, args, environ);
```

### Signal Handling

| Signal | Handler |
|--------|---------|
| SIGCHLD | Reap with `waitpid(-1, NULL, WNOHANG)` |
| SIGPIPE | Ignored |

### Single Instance

```c
fd = open("/tmp/qua-socket-daemon.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
flock(fd, LOCK_EX | LOCK_NB);  // fails if already running
```

## Startup Sequence

1. `init_history_path()` - resolve config dir
2. `find_playable_from_history()` - populate last_played
3. Acquire lock file
4. `unlink()` stale socket
5. `socket()` + `bind()` + `listen()`
6. Setup spawn attributes
7. Setup signal handlers
8. Accept loop

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
