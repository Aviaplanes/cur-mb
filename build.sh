#!/bin/sh
set -eu
# Zig's Clang/LLD cross compiler and bundled MinGW headers/libraries.
# Tested with Zig 0.14.0-dev.2456+a68119f8f.
: "${ZIG:=zig}"
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -s \
    -DUNICODE -D_UNICODE -municode -Wl,--subsystem,windows -static \
    curmb.cpp -o curmb.exe -lgdi32 -lshell32 -lshcore -lwinmm -ladvapi32
