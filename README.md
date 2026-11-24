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
