#!/bin/sh
musl-gcc -std=c23 -O2 -static -flto -fno-pie -march=native -o qua-send qua-send.c
