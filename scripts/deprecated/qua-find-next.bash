#!/bin/bash

WRITE_TO_FILE=0
OFFSET_ARG=1 # Default offset is 1 (next song)

# Check for the -t flag and capture the offset. 
# We look for -t anywhere, and the first argument not equal to -t is the offset.

for arg in "$@"; do
    if [[ "$arg" == "-t" ]]; then
        WRITE_TO_FILE=1
    elif [[ "$arg" =~ ^-?[0-9]+$ ]]; then # Checks if the argument is an integer (e.g., -1, 2, 0)
        OFFSET_ARG="$arg"
    fi
done

qua_find() {
    FILE="$1"
    OFFSET="$2" 
    DIR=$(dirname "$FILE")
    
    # Collect all audio files sorted
    files=()
    for f in "$DIR"/*.flac "$DIR"/*.mp3 "$DIR"/*.wv "$DIR"/*.m4a; do
        [ -f "$f" ] && files+=("$f")
    done
    
    # Sort the array
    IFS=$'\n' files=($(sort <<<"${files[*]}"))
    unset IFS
    
    # Find current file and return offset song (wraps around)
    for i in "${!files[@]}"; do
        if [ "${files[$i]}" = "$FILE" ]; then
            # Calculate new index with wrapping (handles negative offsets)
            total=${#files[@]}
            next_index=$(( (i + OFFSET) % total ))
            
            # Handle negative modulo (bash doesn't wrap negatives correctly)
            [ $next_index -lt 0 ] && next_index=$(( next_index + total ))
            
            echo "${files[$next_index]}"
            return 0
        fi
    done
    
    # If file not found, return first file
    [ ${#files[@]} -gt 0 ] && echo "${files[0]}"
}

CURRENT=$(cat /tmp/qua-current-song 2>/dev/null)

# Pass the determined offset (OFFSET_ARG) to qua_find
NEXT=$(qua_find "$CURRENT" "$OFFSET_ARG")

# Always output
echo -n "$NEXT"

# Optionally write to file
[ "$WRITE_TO_FILE" -eq 1 ] && echo -n "$NEXT" > /tmp/qua-current-song
