#include "../trail.h"
#include "../blend.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    static_assert(FrameGapMs(60, false) == 8);
    static_assert(FrameGapMs(120, false) == 4);
    static_assert(FrameGapMs(144, false) == 3);
    static_assert(FrameGapMs(60, true) == 4);
    static_assert(FrameGapMs(120, true) == 2);
    static_assert(FrameGapMs(144, true) == 2);
    static_assert(FrameGapMs(360, true) == 2);

    Copy copies[3] = {{1, 1, 0.25f}, {}, {77, 77, 77.0f}};
    int n = 1;
    // Screen position (-95, 16), hotspot (2, 3), frame origin (-100, 10).
    AppendCursorHead(copies, n, -95, 16, 2, 3, -100, 10);
    assert(n == 2 && copies[1].x == 3 && copies[1].y == 3 && copies[1].a == 1.0f);
    assert(copies[0].x == 1 && copies[0].y == 1 && copies[0].a == 0.25f);
    assert(copies[2].x == 77 && copies[2].y == 77 && copies[2].a == 77.0f);

    // Paint a translucent trail then the head: fully opaque head pixels must
    // win, and transparent pixels must leave the existing trail untouched.
    uint32_t canvas[8 * 8] = {};
    const uint32_t sprite[4] = {0xff102030, 0, 0x80804020, 0xff00ff00};
    const int spans[4] = {0, 2, 0, 2};
    BlendPixels(canvas, 8, sprite, 2, 2, spans, 64, 3, 3);
    const uint32_t beforeTransparent = canvas[3 * 8 + 4];
    BlendPixels(canvas, 8, sprite, 2, 2, spans, static_cast<int>(copies[1].a * 255),
                copies[1].x, copies[1].y);
    assert(canvas[3 * 8 + 3] == sprite[0]);
    assert(canvas[3 * 8 + 4] == beforeTransparent);
    assert(canvas[4 * 8 + 4] == sprite[3]);
    std::puts("Frame intervals and sharp-head placement/blending passed");
}
