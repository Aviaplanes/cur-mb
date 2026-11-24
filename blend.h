#pragma once

#include <cstdint>
#include <emmintrin.h>

// Premultiplied BGRA source-over, bit-identical to the scalar implementation.
// SSE2 is part of the x86-64 baseline, so no CPU feature dispatch is needed.
static inline void BlendPixels(uint32_t* canvas, int canvasWidth,
                               const uint32_t* picture, int pictureWidth, int pictureHeight,
                               const int* spans, int opacity, int ox, int oy) {
    const uint32_t k = static_cast<uint32_t>(opacity) + 1;
    const __m128i zero = _mm_setzero_si128();
    const __m128i scale = _mm_set1_epi16(static_cast<short>(k));
    const __m128i full = _mm_set1_epi32(256);
    for (int y = 0; y < pictureHeight; ++y) {
        uint32_t* dst = canvas + (oy + y) * canvasWidth + ox;
        const uint32_t* src = picture + y * pictureWidth;
        int x = spans[2 * y], end = spans[2 * y + 1];
        for (; x + 4 <= end; x += 4) {
            __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
            __m128i lo = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), scale), 8);
            __m128i hi = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(s, zero), scale), 8);
            s = _mm_packus_epi16(lo, hi);

            // Replicate each pixel's inverse alpha across its four 16-bit channels.
            __m128i inv = _mm_sub_epi32(full, _mm_srli_epi32(s, 24));
            inv = _mm_shufflelo_epi16(inv, _MM_SHUFFLE(2, 2, 0, 0));
            inv = _mm_shufflehi_epi16(inv, _MM_SHUFFLE(2, 2, 0, 0));
            __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + x));
            lo = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero),
                                                _mm_unpacklo_epi32(inv, inv)), 8);
            hi = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero),
                                                _mm_unpackhi_epi32(inv, inv)), 8);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x),
                             _mm_add_epi32(s, _mm_packus_epi16(lo, hi)));
        }
        for (; x < end; ++x) {
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
