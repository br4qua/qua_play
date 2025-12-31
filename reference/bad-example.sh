#!/bin/sh
FILE="$1"

# Player binaries path
PLAYER_16_BIT=qua_player_16
PLAYER_32_BIT=qua_player_32

# Configurations
DEVICE="hw:0,0"			# Playback device
CORES="4"				# Which core(s) you want the player to play on. Example: "3,4,5" or "2". This is zero indexed.
VALID_BIT_DEPTH="16 32" # List of valid Bit-depths your hardware supports.
FORCE_BIT_DEPTH=false      # Force playback at a specific sample rate. Options: false OR user define value
FALLBACK_BIT_DEPTH=32	# Fallback bit-depth, used when detected bit-depth is not within VALID_BIT_DEPTH
VALID_SAMPLE_RATES="44100 48000 88200 96000 176400 192000 352800 384000" # List of valid sample rates
FORCE_SAMPLE_RATE=false # Force playback at a specific sample rate. Options: false OR user define value
FALLBACK_SAMPLE_RATE=96000 # Fallback bit-depth, used when detected bit-depth is not within VALID_BIT_DEPTH

main() {
    setup_format
    wav_conversion
    post_processing
    playback
}

setup_format() {
    # Determining Bit-Depth for playback
    detected_bit_depth=$(sox --i -p "$FILE" 2>/dev/null)
    echo "Detected bit-depth: $detected_bit_depth"
    
    # Todo: Clean up logic a bit -> Fallback Rate, then Detected Rate, then Forced Rate
    if [ "$FORCE_BIT_DEPTH" != "false" ]; then
        playback_bit_depth="$FORCE_BIT_DEPTH"
        echo "Playback using FORCED $playback_bit_depth bits"
    else
        # Check if detected bit depth is valid
        playback_bit_depth="$FALLBACK_BIT_DEPTH"
        for bit_depth in $VALID_BIT_DEPTH; do
            if [ "$detected_bit_depth" = "$bit_depth" ]; then
                playback_bit_depth="$detected_bit_depth"
                break
            fi
        done
        echo "Playback using $playback_bit_depth bits"
    fi
    
    if [ "$playback_bit_depth" = "16" ]; then
        PLAYER="$PLAYER_16_BIT"
    else
        PLAYER="$PLAYER_32_BIT"
    fi
    
    # Determining Sample Rate for playback
    # Todo: Clean up logic a bit -> Fallback Rate, then Detected Rate, then Forced Rate
    detected_sample_rate=$(sox --i -r "$FILE" 2>/dev/null)
    echo "Detected sample rate: $detected_sample_rate Hz"
    if [ "$FORCE_SAMPLE_RATE" != "false" ]; then
        playback_sample_rate="$FORCE_SAMPLE_RATE"
        echo "Playback using FORCED $playback_sample_rate Hz"
    else
        # Check if detected sample rate is valid
        playback_sample_rate="$FALLBACK_SAMPLE_RATE"
        for rate in $VALID_SAMPLE_RATES; do
            if [ "$detected_sample_rate" = "$rate" ]; then
                playback_sample_rate="$detected_sample_rate"
                break
            fi
        done
        echo "Playback using $playback_sample_rate Hz"
    fi
}

wav_conversion() {
    # Generate temp filename in shared memory
    TEMP_WAV_INTERMEDIATE="/dev/shm/sa_temp_raw_$$.wav"
    EXT="${FILE##*.}"
    # Perform conversion based on file format
    case "$EXT" in
        "flac")
            flac -d -c "$FILE" > "$TEMP_WAV_INTERMEDIATE"
        ;;
        "wv")
            wvunpack "$FILE" -o "$TEMP_WAV_INTERMEDIATE"
        ;;
        "ape")
            mac "$FILE" -d "$TEMP_WAV_INTERMEDIATE"
        ;;
        "mp3")
            mpg123 -w "$TEMP_WAV_INTERMEDIATE" "$FILE"
        ;;
        "opus")
        	# or --no-dither, TODO maybe add this as a generic flag up top
            opusdec --force-wav "$FILE" "$TEMP_WAV_INTERMEDIATE"
        ;;
        "ogg")
            oggdec "$FILE" -o "$TEMP_WAV_INTERMEDIATE"
        ;;
        "m4a"|"mp4"|"wav"|"aiff")
            ffmpeg -v quiet -i "$FILE" -f wav "$TEMP_WAV_INTERMEDIATE"
        ;;
        *)
            echo "Unsupported file format: $EXT"
            exit 1
        ;;
    esac
}



post_processing() {
    # Generate final output filename
    TEMP_WAV_PLAYBACK="/dev/shm/sa_temp_playback_$$.wav"

    # Check the channel count of the intermediate file
    detected_channels=$(soxi -c "$TEMP_WAV_INTERMEDIATE" 2>/dev/null)
    
    # Set the REMIX_COMMAND variable
    REMIX_COMMAND=""
    
 REMIX_COMMAND=""
        
        # --- Logic Block 1: Mono (1 Channel) to Stereo (Dual-Mono) ---
        if [ "$detected_channels" -eq 1 ]; then
            echo "Input is Mono. Applying Dual-Mono conversion with -6dB gain."
            # Use 'channels 2' to duplicate the mono channel to two channels.
            # Add 'gain -6' to compensate for the acoustic summation (volume doubling).
            REMIX_COMMAND="channels 2"
            
        # --- Logic Block 2: 6-Channel (5.1) Downmix to Stereo ---
        elif [ "$detected_channels" -eq 6 ]; then
            echo "Input is 6-Channel (5.1). Downmixing to Stereo with gain adjustments."
            
            # Standard passive downmix formula (L, R, C, LFE, SL, SR -> L', R'):
            # L' = L + 0.707*C + 0.707*SL
            # R' = R + 0.707*C + 0.707*SR
            # This prevents clipping and maintains general loudness perception.
            playback_bit_depth=32
            REMIX_COMMAND="remix 1,3v0.707,5v0.707 2,3v0.707,6v0.707"
        # --- Logic Block 3: All other channel counts (2, 3, 4, 5, 7, 8, etc.) ---
        else
            echo "Input is Stereo or Non-standard Multi-channel ($detected_channels channels). No adjustment needed."
        fi

    # Apply all SoX processing in one go
    # The REMIX_COMMAND is included here, but only contains effects if the file was mono.
    sox -V3 "$TEMP_WAV_INTERMEDIATE" -t wav -b "$playback_bit_depth" -e signed-integer \
        "$TEMP_WAV_PLAYBACK" $REMIX_COMMAND rate -v "$playback_sample_rate" pad 0.2 0.1

}

playback() {
	dunst
	PLAYER_NAME="qua_player_${playback_bit_depth}_${playback_sample_rate}"
    
    # 2. Reassign the final executable name to $PLAYER for use in the command
    PLAYER="$PLAYER_NAME"
    notify-send -a "$PLAYER"
	HUGE_LIB="/usr/lib64/libhugetlbfs.so"
    echo "Starting playback using: $PLAYER"
    nohup setsid taskset -c "$CORES" "$PLAYER" "$TEMP_WAV_PLAYBACK" "$DEVICE" $$  </dev/null >/dev/null 2>&1 &
#    "$PLAYER" "$TEMP_WAV_PLAYBACK" "$DEVICE" $$
}

cleanup() {
    echo "Cleaning up temporary files..."
    sleep 2
    rm -f /dev/shm/sa_temp_*_$$.wav 2>/dev/null
    echo "Cleanup complete"
}

# Set trap to catch interrupts and exits
trap cleanup EXIT INT TERM

# Call main function
main
