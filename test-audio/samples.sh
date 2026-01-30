#!/bin/bash

# Cut a file so it can be used to train the PGO versions.
# Can be less if you wish build less versions.

# TODO Verify working + find royality free music.

INPUT="$1"
LENGTH="${2:-45}" # default to 45 seconds if not provided

if [[ ! -f "$INPUT" ]]; then
    echo "Usage: ./samples.sh input_music_file.wav [length_in_seconds]"
    exit 1
fi

echo "Converting $INPUT to training files (${LENGTH} seconds each)..."

sox "$INPUT" -r 44100 -b 16 16bit_44100.wav trim 0 "$LENGTH"
sox "$INPUT" -r 48000 -b 16 16bit_48000.wav trim 0 "$LENGTH"
sox "$INPUT" -r 88200 -b 16 16bit_88200.wav trim 0 "$LENGTH"
sox "$INPUT" -r 96000 -b 16 16bit_96000.wav trim 0 "$LENGTH"
sox "$INPUT" -r 192000 -b 16 32bit_192000.wav trim 0 "$LENGTH"

sox "$INPUT" -r 44100 -b 32 32bit_44100.wav trim 0 "$LENGTH"
sox "$INPUT" -r 48000 -b 32 32bit_48000.wav trim 0 "$LENGTH"
sox "$INPUT" -r 88200 -b 32 32bit_88200.wav trim 0 "$LENGTH"
sox "$INPUT" -r 96000 -b 32 32bit_96000.wav trim 0 "$LENGTH"
sox "$INPUT" -r 192000 -b 32 32bit_192000.wav trim 0 "$LENGTH"


echo "✅ Done!"
ls -lh *.wav
