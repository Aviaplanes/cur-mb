# curmb

Standalone Windows 11 x64 cursor motion blur. No Windhawk or additional runtime
is required. Run **one** of these files:

| File | Effect |
| --- | --- |
| `curmb.exe` | Baseline: motion blur with the settings below. |
| `curmb_1.exe` | Baseline + a sharp cursor sprite at the latest sampled position while the real cursor is hidden. |
| `curmb_2.exe` | `curmb_1.exe` + more frequent frame scheduling: approximately a quarter of a refresh period, with a 2 ms minimum, measured from the start of a successful frame. This may use more CPU and does not guarantee lower display latency. |

All variants use the same single-instance lock; stop the running one before
trying another. Each launches a second instance of the same executable as a
watchdog. End the primary process in Task Manager to stop it. The watchdog
restores system cursors if the primary process is terminated while they are
hidden. If both are forcibly terminated together, launch a variant again to
restore any cursor scheme left hidden.

The values from `settings.txt` are compiled into all executables: opacity 45%,
minimum blur speed 50 cm/s, trail duration 20 ms, hide above 90 cm/s, and pause
for fullscreen applications. No settings file is needed at runtime. The sharp
sprite is drawn only while this application is hiding the real cursor; it uses
the last sampled position, so it is not a hardware cursor and does not remove
Windows compositor latency.

The overlay is an ordinary topmost window; without injection or UIAccess it
cannot appear above protected Windows surfaces such as Start or the secure
desktop. Fullscreen detection uses Windows notification state. The variants
have not been visually tested or latency-measured on Windows.

## Build

Run `./build.sh` with Zig 0.14.0-dev.2456+a68119f8f in PATH (or set `ZIG` to
its path). Zig provides the Clang compiler, LLD linker, and MinGW Windows
headers and import libraries. The script builds all three x64 GUI executables.

## Tests

On an x86-64 machine:

```sh
c++ -std=c++17 -O2 tests/blend_test.cpp -o /tmp/curmb-blend-test
/tmp/curmb-blend-test --bench
c++ -std=c++17 -O2 tests/trail_test.cpp -o /tmp/curmb-trail-test
/tmp/curmb-trail-test
```

The blend test compares the entire destination buffer for 30,000 randomized
cases against the original scalar calculation. Its optional benchmark measures
only the blend kernel, not Windows rendering, input latency, or total CPU use.
The trail test checks frame interval calculations, head placement and blending;
it does not exercise Win32 cursor hiding or DWM presentation.
