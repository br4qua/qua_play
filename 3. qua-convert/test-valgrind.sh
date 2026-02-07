#!/bin/sh

INPUT='/home/free2/Music/pop-newer/Lady Gaga/2008 - The Fame (hdtracks + jpdeluxe)/01 Just Dance.wv'
OUTPUT="/dev/shm/test-output.wav"

valgrind --leak-check=full --track-origins=yes ./qua-convert "$INPUT" "$OUTPUT"

rm -f "$OUTPUT"
