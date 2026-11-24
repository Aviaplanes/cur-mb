#include "../blend.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static void Reference(uint32_t* canvas, int stride, const uint32_t* pic, int w, int h,
                      const int* spans, int opacity, int ox, int oy) {
    uint32_t k = static_cast<uint32_t>(opacity) + 1;
    for (int y = 0; y < h; ++y) {
        uint32_t* dst = canvas + (oy + y) * stride + ox;
        const uint32_t* src = pic + y * w;
        for (int x = spans[2 * y]; x < spans[2 * y + 1]; ++x) {
            uint32_t s = src[x];
            if (!s) continue;
            s = ((s & 0xFF00FF) * k >> 8 & 0xFF00FF) |
                ((s >> 8 & 0xFF00FF) * k & 0xFF00FF00);
            uint32_t inv = 256 - (s >> 24), d = dst[x];
            dst[x] = s + (((d & 0xFF00FF) * inv >> 8 & 0xFF00FF) |
                          ((d >> 8 & 0xFF00FF) * inv & 0xFF00FF00));
        }
    }
}

static bool Check() {
    std::mt19937 rng(0x4355524d);
    for (int test = 0; test < 30000; ++test) {
        int w = 1 + rng() % 129, h = 1 + rng() % 65;
        int stride = w + 1 + rng() % 16, ox = rng() % (stride - w + 1), oy = rng() % 4;
        std::vector<uint32_t> pic(w * h), reference(stride * (h + oy + 1)), actual;
        std::vector<int> spans(2 * h);
        for (auto& v : pic) v = (rng() % 4) ? rng() : 0;
        for (auto& v : reference) v = rng();
        actual = reference;
        for (int y = 0; y < h; ++y) {
            int left = rng() % (w + 1);
            spans[2 * y] = left;
            spans[2 * y + 1] = left + rng() % (w - left + 1);
        }
        int alpha = 1 + test % 255;
        Reference(reference.data(), stride, pic.data(), w, h, spans.data(), alpha, ox, oy);
        BlendPixels(actual.data(), stride, pic.data(), w, h, spans.data(), alpha, ox, oy);
        if (actual != reference) {
            std::fprintf(stderr, "Mismatch at test %d, width %d, height %d, opacity %d\n", test, w, h, alpha);
            return false;
        }
    }
    return true;
}

static void Benchmark(int size) {
    const int stride = size * 4, height = size * 2, loops = 100000;
    std::vector<uint32_t> pic(size * size, 0), old(stride * height, 0), simd = old;
    std::vector<int> spans(size * 2);
    // Approximate arrow-shaped rows, with holes and partly transparent edges.
    for (int y = 0; y < size; ++y) {
        int left = y / 8, right = (y < size * 2 / 3) ? y / 2 + 2 : size / 2;
        if (right > size) right = size;
        spans[y * 2] = left;
        spans[y * 2 + 1] = right;
        for (int x = left; x < right; ++x)
            pic[y * size + x] = ((x + y) % 7 == 0) ? 0 : 0xd0606060;
    }
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < loops; ++i)
        Reference(old.data(), stride, pic.data(), size, size, spans.data(), 1 + i % 115, i % size, i % (size / 2));
    auto middle = std::chrono::steady_clock::now();
    for (int i = 0; i < loops; ++i)
        BlendPixels(simd.data(), stride, pic.data(), size, size, spans.data(), 1 + i % 115, i % size, i % (size / 2));
    auto end = std::chrono::steady_clock::now();
    double before = std::chrono::duration<double, std::milli>(middle - start).count();
    double after = std::chrono::duration<double, std::milli>(end - middle).count();
    if (old != simd) { std::fprintf(stderr, "Benchmark mismatch\n"); std::abort(); }
    std::printf("%dx%d: scalar %.1f ms, SSE2 %.1f ms (%.2fx)\n", size, size, before, after, before / after);
}

int main(int argc, char** argv) {
    if (!Check()) return 1;
    std::puts("30,000 randomized blend comparisons passed");
    if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) {
        Benchmark(32);
        Benchmark(64);
        Benchmark(128);
    }
}
