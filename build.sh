#!/bin/sh
set -eu
# Zig's Clang/LLD cross compiler and bundled MinGW headers/libraries.
# Tested with Zig 0.14.0-dev.2456+a68119f8f.
: "${ZIG:=zig}"
build() {
    output=$1
    shift
    "$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -s \
        -DUNICODE -D_UNICODE -municode -Wl,--subsystem,windows -static \
        "$@" curmb.cpp -o "$output" -lgdi32 -lshell32 -lshcore -lwinmm -ladvapi32
}
build curmb.exe
build curmb_1.exe -DCURMB_SHARP_HEAD=1
build curmb_2.exe -DCURMB_SHARP_HEAD=1 -DCURMB_FAST_FRAMES=1
