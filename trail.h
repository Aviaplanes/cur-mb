#pragma once

// Cursor copy location relative to the frame and opacity.
struct Copy { int x, y; float a; };

// Baseline: half a refresh, at least 3 ms. Fast variant: quarter of a
// refresh, at least 2 ms, to limit the CPU cost on high-refresh monitors.
constexpr int FrameGapMs(int hz, bool fast) {
    const int interval = (fast ? 250 : 500) / hz;
    const int minimum = fast ? 2 : 3;
    return interval > minimum ? interval : minimum;
}

// The caller reserves one slot for the head and draws copies in array order,
// so the opaque cursor sprite is blended on top of the translucent trail.
inline void AppendCursorHead(Copy* copies, int& count, int x, int y,
                             int hotspotX, int hotspotY, int minX, int minY) {
    copies[count++] = {x - hotspotX - minX, y - hotspotY - minY, 1.0f};
}
