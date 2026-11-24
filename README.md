# curmb

Standalone Windows 11 x64 cursor motion blur. Run `curmb.exe`; it has no
visible interface. End the primary `curmb.exe` process in Task Manager to stop
it. A second instance of the same executable watches the primary process and
restores the system cursors if it is terminated while they are hidden. If both
processes are forcibly terminated at once, launch `curmb.exe` again to restore
any cursor scheme left hidden.

The values from `settings.txt` are compiled into the executable: opacity 45%,
minimum blur speed 50 cm/s, trail duration 20 ms, hide above 90 cm/s, and pause
for fullscreen applications. No settings file is needed at runtime.

The overlay is an ordinary topmost window; without injection or UIAccess it
cannot appear above protected Windows surfaces such as Start or the secure
desktop. Fullscreen detection uses Windows notification state.

## Build

Run `./build.sh` with Zig 0.14.0-dev.2456+a68119f8f in PATH (or set `ZIG` to
its path). Zig provides the Clang compiler, LLD linker, and MinGW Windows
headers and import libraries. The resulting x64 GUI executable depends only on
Windows system libraries; it does not require Windhawk or a separate runtime.

## Pixel kernel check

The blend kernel uses x86-64 baseline SSE2, processing four BGRA pixels per
iteration; a scalar tail handles short rows. To compare it with the original
scalar calculation and run a synthetic CPU benchmark on an x86-64 machine:

```sh
c++ -std=c++17 -O2 tests/blend_test.cpp -o /tmp/curmb-blend-test
/tmp/curmb-blend-test --bench
```

The test compares the entire destination buffer for 30,000 randomized cases,
including arbitrary pixels, transparent pixels, row spans, offsets and all
opacity values. The benchmark measures only the blend kernel, not Windows
rendering, input latency or total application CPU usage.
