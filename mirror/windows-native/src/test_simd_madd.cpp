#include <windows.h>
#include <tmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>
#include <cstdint>
#include <vector>
#include <stdio.h>

void BgraToNv12_SSSE3(const uint8_t* bgra, UINT stride, UINT width, UINT height, uint8_t* outNv12) {
    uint8_t* yPlane = outNv12;
    uint8_t* uvPlane = outNv12 + (size_t)width * height;

    // Y = (66*R + 129*G + 25*B + 128) >> 8 + 16
    // In BGRA order: B is 0, G is 1, R is 2, A is 3
    // Maddubs with [25, 129, 66, 0] gives:
    // (B * 25 + G * 129) as word 0
    // (R * 66 + A * 0) as word 1
    const __m128i yWeights = _mm_setr_epi8(25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0);
    const __m128i yBias    = _mm_set1_epi32(128 + (16 << 8));

    for (UINT y = 0; y < height; y++) {
        const uint8_t* srcRow = bgra + (size_t)y * stride;
        uint8_t* dstY = yPlane + (size_t)y * width;
        uint8_t* dstUV = uvPlane + ((size_t)y / 2) * width;
        bool doUV = (y % 2 == 0);

        UINT x = 0;
        // Process 4 pixels at a time
        for (; x + 4 <= width; x += 4) {
            __m128i bgraPix = _mm_loadu_si128((const __m128i*)(srcRow + x * 4)); // 4 BGRA pixels

            // Maddubs: pairs of bytes (B*25 + G*129) and (R*66 + 0)
            __m128i pairs = _mm_maddubs_epi16(bgraPix, yWeights); // 8 16-bit integers: [BG0, R0, BG1, R1, BG2, R2, BG3, R3]

            // Sum adjacent words: BG0 + R0, BG1 + R1, ... using madd with 1
            __m128i ones = _mm_set1_epi16(1);
            __m128i y32 = _mm_madd_epi16(pairs, ones); // 4 32-bit integers: (BG0 + R0), (BG1 + R1)...

            // Add bias and shift right by 8
            y32 = _mm_add_epi32(y32, yBias);
            y32 = _mm_srli_epi32(y32, 8);

            // Pack 32-bit to 16-bit, then to 8-bit
            __m128i y16 = _mm_packs_epi32(y32, y32);
            __m128i y8  = _mm_packus_epi16(y16, y16);

            // Store 4 bytes of Y
            *(uint32_t*)(dstY + x) = (uint32_t)_mm_cvtsi128_si32(y8);

            if (doUV) {
                // For chroma, compute 2 pixels: x, x+2
                uint32_t px0 = *(const uint32_t*)(srcRow + x * 4);
                uint32_t px2 = *(const uint32_t*)(srcRow + (x + 2) * 4);

                int b0 = px0 & 0xFF, g0 = (px0 >> 8) & 0xFF, r0 = (px0 >> 16) & 0xFF;
                int b2 = px2 & 0xFF, g2 = (px2 >> 8) & 0xFF, r2 = (px2 >> 16) & 0xFF;

                dstUV[x + 0] = (uint8_t)(((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) + 128);
                dstUV[x + 1] = (uint8_t)(((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) + 128);

                dstUV[x + 2] = (uint8_t)(((-38 * r2 - 74 * g2 + 112 * b2 + 128) >> 8) + 128);
                dstUV[x + 3] = (uint8_t)(((112 * r2 - 94 * g2 - 18 * b2 + 128) >> 8) + 128);
            }
        }

        // Remainder
        for (; x < width; x++) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];
            dstY[x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (doUV && (x % 2 == 0)) {
                dstUV[x]     = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                dstUV[x + 1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
}

int main() {
    UINT width = 1920, height = 1080, stride = 1920 * 4;
    std::vector<uint8_t> bgra(stride * height, 180);
    std::vector<uint8_t> nv12(width * height * 3 / 2);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    BgraToNv12_SSSE3(bgra.data(), stride, width, height, nv12.data());

    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 100; i++) {
        BgraToNv12_SSSE3(bgra.data(), stride, width, height, nv12.data());
    }
    QueryPerformanceCounter(&t1);

    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / (freq.QuadPart * 100.0);
    printf("BgraToNv12_SSSE3 took: %.3f ms per 1080p frame!\n", ms);
    return 0;
}
