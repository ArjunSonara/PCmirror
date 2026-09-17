#include <windows.h>
#include <immintrin.h>
#include <cstdint>
#include <vector>
#include <stdio.h>

void BgraToNv12_AVX2(const uint8_t* bgra, UINT stride, UINT width, UINT height, uint8_t* outNv12) {
    uint8_t* yPlane = outNv12;
    uint8_t* uvPlane = outNv12 + (size_t)width * height;

    const __m256i yWeights = _mm256_setr_epi8(
        25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0,
        25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0,  25, 129, 66, 0
    );
    const __m256i yBias = _mm256_set1_epi32(128 + (16 << 8));
    const __m256i ones  = _mm256_set1_epi16(1);

    for (UINT y = 0; y < height; y++) {
        const uint8_t* srcRow = bgra + (size_t)y * stride;
        uint8_t* dstY = yPlane + (size_t)y * width;
        uint8_t* dstUV = uvPlane + ((size_t)y / 2) * width;
        bool doUV = (y % 2 == 0);

        UINT x = 0;
        // Process 8 pixels (32 bytes) at a time using 256-bit registers
        for (; x + 8 <= width; x += 8) {
            __m256i bgraPix = _mm256_loadu_si256((const __m256i*)(srcRow + x * 4));

            __m256i pairs = _mm256_maddubs_epi16(bgraPix, yWeights);
            __m256i y32   = _mm256_madd_epi16(pairs, ones);
            y32 = _mm256_add_epi32(y32, yBias);
            y32 = _mm256_srli_epi32(y32, 8);

            // Pack 32-bit integers to 16-bit, then to 8-bit
            __m256i y16 = _mm256_packs_epi32(y32, y32);
            // Permute to fix AVX2 cross-lane pack
            y16 = _mm256_permute4x64_epi64(y16, _MM_SHUFFLE(3, 1, 2, 0));
            __m256i y8  = _mm256_packus_epi16(y16, y16);

            // Store 8 bytes of Y
            uint64_t y8val = (uint64_t)_mm_cvtsi128_si64(_mm256_castsi256_si128(y8));
            *(uint64_t*)(dstY + x) = y8val;

            if (doUV) {
                for (UINT k = 0; k < 8; k += 2) {
                    uint32_t px = *(const uint32_t*)(srcRow + (x + k) * 4);
                    int b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF;
                    dstUV[x + k + 0] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                    dstUV[x + k + 1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                }
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

    BgraToNv12_AVX2(bgra.data(), stride, width, height, nv12.data());

    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 100; i++) {
        BgraToNv12_AVX2(bgra.data(), stride, width, height, nv12.data());
    }
    QueryPerformanceCounter(&t1);

    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / (freq.QuadPart * 100.0);
    printf("BgraToNv12_AVX2 took: %.3f ms per 1080p frame!\n", ms);
    return 0;
}
