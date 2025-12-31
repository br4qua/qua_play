#!/bin/sh

NUKE_SUFFIX="" # PGO player pgo suffix or .pgo8.bolt4
DEVICE="hw:0,0"			# Playback device, for valid devices see qua-valid-device
CORE="4"				# Which core player to be pinned on.
BIT_DEPTH_VALID="16 32" # List of valid Bit-depths your hardware supports.
BIT_DEPTH_OVERRIDE=        # Force playback at a specific sample rate. Options: false OR user define value
BIT_DEPTH_FALLBACK=32	# Fallback bit-depth, when detected bit-depth is not in BIT_DEPTH_VALID
SAMPLE_RATE_VALID="44100 48000 88200 96000 176400 192000 352800 384000" # List of valid sample rates
SAMPLE_RATE_OVERRIDE=      # Force playback at a specific sample rate. Options: empty string OR sample rate Example: "96000"
SAMPLE_RATE_FALLBACK=96000 # Fallback bit-depth, used when detected bit-depth is not within BIT_DEPTH_VALID
PADDING_HEAD=0.00 # Add seconds of slience  at the start of the song
PADDING_TAIL=0.00 # Add silence at the end of the song
CACHE_LIMIT=2147483648 # Cache size for previously decoded song in RAM, in Bytes. Default (2 GB)

update_mpris_metadata() {
TEMP_DIR="${FILE%/*}"
dbus-send --session --type=method_call --dest=org.mpris.MediaPlayer2.qua /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.UpdateMetadata string:"${FILE##*/}" string:"${TEMP_DIR##*/}" &
}

# Source Config file
# CONFIG_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/qua-player/config"
# [ -f "$CONFIG_FILE" ] && . "$CONFIG_FILE"

CURRENT_SONG_FILE="$HOME/.config/qua-player/current-song"

# Parse flags
OFFSET=""
FILE_ARG=""


while [ $# -gt 0 ]; do
    case "$1" in
        -n)
            OFFSET="$2"
            shift 2
        ;;
        -p)
            # Negate the value for backward movement
            OFFSET="-$2"
            shift 2
        ;;
        *)
            FILE_ARG="$1"
            shift
        ;;
    esac
done

FILE=""

if [ -n "$OFFSET" ] && [ "$OFFSET" != "0" ]; then
    # Read current song
    read -r CURRENT < "$CURRENT_SONG_FILE" 2>/dev/null
    
    # echo "DEBUG: CURRENT='$CURRENT'"
    
    # Extract directory using parameter expansion (POSIX)
    case "$CURRENT" in
        */*) DIR="${CURRENT%/*}" ;;
        *) DIR="." ;;
    esac
    
    echo "DEBUG: DIR='$DIR'"
    
    # Collect files into positional parameters - glob is already sorted
    set --
    for f in "$DIR"/*.flac "$DIR"/*.mp3 "$DIR"/*.wv "$DIR"/*.ape "$DIR"/*.opus "$DIR"/*.ogg "$DIR"/*.m4a "$DIR"/*.mp4 "$DIR"/*.wav "$DIR"/*.aiff "$DIR"/*.aif; do
        [ -f "$f" ] && set -- "$@" "$f"
    done
    
    # Total files is now $#
    file_count="$#"
    
    if [ "$file_count" -eq 0 ]; then
        echo "No files found in directory"
        exit 1
    else
        # Find current file index in positional parameters
        found=0
        current_index=0
        i=1
        
        for check_file in "$@"; do
            if [ "$check_file" = "$CURRENT" ]; then
                current_index="$i"
                found=1
                break
            fi
            i=$((i + 1))
        done
        
        if [ "$found" -eq 1 ]; then
            # Calculate wrapped index (convert to 0-based, apply offset, wrap, convert back to 1-based)
            next_index=$(( (current_index - 1 + OFFSET) % file_count ))
            [ "$next_index" -lt 0 ] && next_index=$(( next_index + file_count ))
            next_index=$((next_index + 1))
            
            # Extract the nth positional parameter
            i=1
            for f in "$@"; do
                if [ "$i" -eq "$next_index" ]; then
                    FILE="$f"
                    break
                fi
                i=$((i + 1))
            done
        else
            # Use first file if current not found
            FILE="$1"
        fi
    fi
    
    elif [ -n "$FILE_ARG" ]; then
    FILE="$FILE_ARG"
else
    # Read current song (replay last)
    read -r FILE < "$CURRENT_SONG_FILE" 2>/dev/null
fi

echo "Selected file: $FILE"

# Exit if no file selected
if [ -z "$FILE" ] || [ ! -f "$FILE" ]; then
    echo "Error: No valid file to play"
    exit 1
fi
FILE_BASENAME="${FILE##*/}"
# clear_previous_instances
PIDS=$(pgrep qua-player)
if [ -n "$PIDS" ]; then
    kill -9 $PIDS
    for pid in $PIDS; do
        while kill -0 $pid 2>/dev/null; do
            sleep 0.1
        done
    done
fi
# stop_system_services
pkill -9 picom &
systemctl --user stop pipewire-pulse.socket pipewire.socket pipewire-pulse.service wireplumber.service pipewire.service

# Generate cache key
# Cache key: file mtime + inode + script mtime - ensures uniqueness and invalidates on file/script changes

CACHE_PREFIX="/dev/shm/qua-cache/qua-temp-playback-$(stat -c '%i-%Y' "$FILE")-$(stat -c '%Y' "$0")"
# Check for any cached version with glob pattern
for CACHE_FILE in "$CACHE_PREFIX"-*.wav; do
    if [ -f "$CACHE_FILE" ]; then
        BASENAME="${CACHE_FILE##*/}"
        BASENAME="${BASENAME%.wav}"
        playback_sample_rate="${BASENAME##*-}"
        BASENAME="${BASENAME%-*}"
        playback_bit_depth="${BASENAME##*-}"
        
        TEMP_WAV_PLAYBACK="$CACHE_FILE"
        EXT="${FILE##*.}"
        # notify-send "Audio Info (Cached)" "$(date +%T)\nFormat: $EXT\nBit-depth: $playback_bit_depth\nSample rate: $playback_sample_rate Hz" &
        
        # playback
        PLAYER_NAME="qua-player-${playback_bit_depth}-${playback_sample_rate}"
        NUKE_PLAYER="$PLAYER_NAME$NUKE_SUFFIX"
        if command -v "$NUKE_PLAYER" >/dev/null 2>&1; then
            PLAYER="$NUKE_PLAYER"
        else
            PLAYER="$PLAYER_NAME"
        fi
        PLAYER_FULL=$(which "$PLAYER")
        notify-send "$PLAYER"$'\n'"$FILE_BASENAME" &
        SONG_PATH=$(head -n1 "$CURRENT_SONG_FILE")
        
        echo "$FILE" > "$CURRENT_SONG_FILE"
        update_mpris_metadata "$(head -n1 "$CURRENT_SONG_FILE")" &
        qua-bare-launcher $CORE "$(realpath "$PLAYER_FULL")" "$TEMP_WAV_PLAYBACK" "$DEVICE" &
        exit 0
    fi
done

# cleanup and cache setup (cache 1gb)
mkdir -p /dev/shm/qua-cache
CACHE_SIZE=$(du -sb /dev/shm/qua-cache 2>/dev/null | cut -f1)
[ "$CACHE_SIZE" -gt "$CACHE_LIMIT" ] && rm -rf /dev/shm/qua-cache/* && mkdir -p /dev/shm/qua-cache

# No cache - do full conversion
# wav_conversion - Decode to intermediate WAV first
TEMP_WAV_INTERMEDIATE="/dev/shm/qua-temp-raw-$$.wav"
EXT="${FILE##*.}"
case "$EXT" in
    "flac")
    flac -d "$FILE" -o "$TEMP_WAV_INTERMEDIATE" ;;
    "wv")
    wvunpack "$FILE" -o "$TEMP_WAV_INTERMEDIATE" ;;
    "ape")
    mac "$FILE" -d "$TEMP_WAV_INTERMEDIATE" ;;
    "mp3")
    mpg123 -w "$TEMP_WAV_INTERMEDIATE" "$FILE" ;;
    "opus")
        # or --no-dither
    opusdec --force-wav "$FILE" "$TEMP_WAV_INTERMEDIATE" ;;
    "ogg")
    oggdec "$FILE" -o "$TEMP_WAV_INTERMEDIATE" ;;
    "m4a"|"mp4"|"wav"|"aiff"|"aif")
    ffmpeg -v quiet -i "$FILE" -f wav "$TEMP_WAV_INTERMEDIATE" ;;
    *)
        echo "Unsupported file format: ${FILE##*.}"
        exit 1
    ;;
esac

# setup_format - Now detect from the decoded WAV
detected_bit_depth=$(soxi -p "$TEMP_WAV_INTERMEDIATE" 2>/dev/null)
detected_sample_rate=$(soxi -r "$TEMP_WAV_INTERMEDIATE" 2>/dev/null)

# notify-send "Audio Info" "$(date +%T)\nFormat: $EXT\nBit-depth: $detected_bit_depth\nSample rate: $detected_sample_rate Hz" &


# Determine playback bit depth
# Todo: Clean up logic a bit -> Fallback Rate, then Detected Rate, then Forced Rate
if [ -n "$BIT_DEPTH_OVERRIDE" ]; then
    playback_bit_depth="$BIT_DEPTH_OVERRIDE"
    #   echo "Playback using FORCED $playback_bit_depth bits"
else
    # Check if detected bit depth is valid
    playback_bit_depth="$BIT_DEPTH_FALLBACK"
    for bit_depth in $BIT_DEPTH_VALID; do
        if [ "$detected_bit_depth" = "$bit_depth" ]; then
            playback_bit_depth="$detected_bit_depth"
            break
        fi
    done
    #   echo "Playback using $playback_bit_depth bits"
fi

# Select player based on bit depth
if [ "$playback_bit_depth" = "16" ]; then
    PLAYER="$PLAYER_16_BIT"
else
    PLAYER="$PLAYER_32_BIT"
fi

# Determine playback sample rate
# Todo: Clean up logic a bit -> Fallback Rate, then Detected Rate, then Forced Rate
if [ -n "$SAMPLE_RATE_OVERRIDE" ]; then
    playback_sample_rate="$SAMPLE_RATE_OVERRIDE"
    #    echo "Playback using FORCED $playback_sample_rate Hz"
else
    # Check if detected sample rate is valid
    playback_sample_rate="$SAMPLE_RATE_FALLBACK"
    for rate in $SAMPLE_RATE_VALID; do
        if [ "$detected_sample_rate" = "$rate" ]; then
            playback_sample_rate="$detected_sample_rate"
            break
        fi
    done
    #    echo "Playback using $playback_sample_rate Hz"
fi

# post_processing - only if not cached
if [ ! -f "$CACHE_FILE" ]; then
    CACHE_FILE="$CACHE_PREFIX-${playback_bit_depth}-${playback_sample_rate}.wav"
    TEMP_WAV_PLAYBACK="$CACHE_FILE"
    DETECTED_CHANNELS=$(soxi -c "$TEMP_WAV_INTERMEDIATE" 2>/dev/null)
    REMIX_COMMAND=""
    case "$DETECTED_CHANNELS" in
        1)
            REMIX_COMMAND="channels 2"
        ;;
        6)
            REMIX_COMMAND="remix 1,3v0.707,5v0.707 2,3v0.707,6v0.707"
            playback_bit_depth=32
        ;;
        *)
        ;;
    esac
    # Apply all SoX processing in one go - output goes to cache
    # sox -V3 "$TEMP_WAV_INTERMEDIATE" -t wav -b "$playback_bit_depth" -e signed-integer \
    # "$TEMP_WAV_PLAYBACK" $REMIX_COMMAND rate -v "$playback_sample_rate" \
    # pad "$PADDING_HEAD" "$PADDING_TAIL" # Increase to see if your hardware keep up during sample rate change

    sox -V3 "$TEMP_WAV_INTERMEDIATE" -t wav -b "$playback_bit_depth" -e signed-integer \
        "$TEMP_WAV_PLAYBACK" $REMIX_COMMAND rate -v "$playback_sample_rate" \
        silence 1 0.1 1% \
        pad "$PADDING_HEAD" "$PADDING_TAIL"
        
    # Clean up intermediate file
    rm -f "$TEMP_WAV_INTERMEDIATE" 2>/dev/null
fi

# playback
PLAYER_NAME="qua-player-${playback_bit_depth}-${playback_sample_rate}"
NUKE_PLAYER="$PLAYER_NAME$NUKE_SUFFIX"
if command -v "$NUKE_PLAYER" >/dev/null 2>&1; then
    PLAYER="$NUKE_PLAYER"
else
    PLAYER="$PLAYER_NAME"
fi
PLAYER_FULL=$(which "$PLAYER")

notify-send "$PLAYER"$'\n'"$FILE_BASENAME" &

echo "$FILE" > "$CURRENT_SONG_FILE"

SONG_PATH=$(head -n1 "$CURRENT_SONG_FILE")
SONG_TITLE=$(basename "$SONG_PATH")
SONG_ARTIST="Unknown Artist"  # or extract from file metadata





# Usage:
update_mpris_metadata "$(head -n1 "$CURRENT_SONG_FILE")" &
qua-bare-launcher $CORE "$(realpath "$PLAYER_FULL")" "$TEMP_WAV_PLAYBACK" "$DEVICE" &
