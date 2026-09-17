#include <windows.h>
#include <cstdint>
#include <vector>
#include <stdio.h>

void ConvertBgraToNv12_Correct(const uint8_t* bgra, UINT stride, UINT width, UINT height, uint8_t* outNv12) {
    uint8_t* yPlane = outNv12;
    uint8_t* uvPlane = outNv12 + (size_t)width * height;

    for (UINT y = 0; y < height; y++) {
        const uint8_t* srcRow = bgra + (size_t)y * stride;
        uint8_t* dstY = yPlane + (size_t)y * width;
        uint8_t* dstUV = uvPlane + ((size_t)y / 2) * width;
        bool doUV = (y % 2 == 0);

        for (UINT x = 0; x < width; x++) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];

            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            dstY[x] = (uint8_t)(yVal < 0 ? 0 : (yVal > 255 ? 255 : yVal));

            if (doUV && (x % 2 == 0)) {
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                dstUV[x]     = (uint8_t)(uVal < 0 ? 0 : (uVal > 255 ? 255 : uVal));
                dstUV[x + 1] = (uint8_t)(vVal < 0 ? 0 : (vVal > 255 ? 255 : vVal));
            }
        }
    }
}

int main() {
    UINT width = 1920, height = 1080, stride = 1920 * 4;
    std::vector<uint8_t> bgra(stride * height, 0);
    std::vector<uint8_t> nv12(width * height * 3 / 2);

    // Test with vibrant green (like the grass field in the user's wallpaper)
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 0] = 30;  // B
        bgra[i + 1] = 180; // G
        bgra[i + 2] = 50;  // R
        bgra[i + 3] = 255; // A
    }

    ConvertBgraToNv12_Correct(bgra.data(), stride, width, height, nv12.data());

    printf("Test Pixel Green (R=50, G=180, B=30):\n");
    printf("  Y  = %u (Expected ~108)\n", nv12[0]);
    printf("  U  = %u (Expected ~80)\n", nv12[width * height]);
    printf("  V  = %u (Expected ~73)\n", nv12[width * height + 1]);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 50; i++) {
        ConvertBgraToNv12_Correct(bgra.data(), stride, width, height, nv12.data());
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / (freq.QuadPart * 50.0);
    printf("Speed: %.2f ms per 1080p frame\n", ms);
    return 0;
}
