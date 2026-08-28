/* host_test.c - Run the FIRMWARE's soft_mjpeg.c on the host to prove it
 * emits a valid, decodable JPEG (mirrors frame_audio_source.c's pattern). */
#include <stdio.h>
#include <stdlib.h>
#include "soft_mjpeg.h"

#define W 256
#define H 240

static uint16_t pat[W * H];

int main(void)
{
    static const uint16_t bars[8] = {
        0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF, 0x0000
    };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px;
            if (y < H - 48) {
                px = bars[x / (W / 8)];
            } else {
                int g  = (x * 255) / (W - 1);
                int r  = (g >> 3) & 0x1F;
                int gg = (g >> 2) & 0x3F;
                int b  = (g >> 3) & 0x1F;
                px = (uint16_t)((r << 11) | (gg << 5) | b);
            }
            pat[y * W + x] = px;
        }
    }

    uint8_t *out = (uint8_t *)malloc(200 * 1024);
    uint32_t n = soft_mjpeg_encode(pat, W, H, 60, out, 200 * 1024);
    if (n == 0) { printf("ENCODE FAILED\n"); return 1; }

    FILE *f = fopen("host_test.jpg", "wb");
    if (!f) { printf("fopen failed\n"); return 1; }
    fwrite(out, 1, n, f);
    fclose(f);
    free(out);
    printf("OK encoded %u bytes\n", n);
    return 0;
}
