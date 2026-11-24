#include "kernels.h"
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
    assert(AppendCursorHead(copies, n, 2, -95, 16, 2, 3, -100, 10));
    assert(n == 2 && copies[1].x == 3 && copies[1].y == 3 && copies[1].a == copies[0].a);
    assert(copies[2].x == 77 && copies[2].y == 77 && copies[2].a == 77.0f);
    assert(!AppendCursorHead(copies, n, 2, 0, 0, 0, 0, 0, 0));
    assert(n == 2);
    n = 0;
    assert(!AppendCursorHead(copies, n, 2, 0, 0, 0, 0, 0, 0));
    assert(n == 0); // no trail, no persistent head

    // The head is just another trail copy: at equal backgrounds it has the
    // same blended pixels, not the fully opaque source cursor.
    uint32_t canvas[8 * 8] = {};
    const uint32_t sprite[4] = {0xff102030, 0, 0x80804020, 0xff00ff00};
    const int spans[4] = {0, 2, 0, 2};
    n = 1;
    assert(AppendCursorHead(copies, n, 2, -95, 16, 2, 3, -100, 10));
    int trailAlpha = static_cast<int>(copies[0].a * 255 + 0.5f);
    int headAlpha = static_cast<int>(copies[1].a * 255 + 0.5f);
    BlendPixels(canvas, 8, sprite, 2, 2, spans, trailAlpha, copies[0].x, copies[0].y);
    BlendPixels(canvas, 8, sprite, 2, 2, spans, headAlpha, copies[1].x, copies[1].y);
    assert(canvas[3 * 8 + 3] == canvas[1 * 8 + 1]);
    assert(canvas[3 * 8 + 3] != sprite[0]);
    assert(canvas[3 * 8 + 4] == 0);
    assert(canvas[4 * 8 + 4] == canvas[2 * 8 + 2]);

    // As the newest trail copy fades, so does the head.
    copies[0].a = 0.02f;
    n = 1;
    assert(AppendCursorHead(copies, n, 2, -95, 16, 2, 3, -100, 10));
    assert(copies[1].a == 0.02f);
    std::puts("Frame intervals, fading trail-head placement, blending and bounds passed");
}
