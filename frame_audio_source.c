/*
 * frame_audio_source.c - content sources for the UVC+UAC composite device
 *
 *   video_source_get_frame() -> the NES PPU framebuffer, encoded to MJPEG
 *                               by InfoNES_LoadFrame() (nes/infones_port.c).
 *   audio_source_fill()      -> NES APU PCM (16-bit stereo, 44100 Hz) when
 *                               the emulator is running; otherwise a 1 kHz
 *                               sine test tone (so the UAC mic side is alive
 *                               even before/without a ROM).
 */
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "uvc_uac.h"
#include "nes/infones_port.h"
#include "nes/soft_mjpeg.h"     /* Phase 1 software MJPEG encoder (test pattern) */

/* Place large static buffers in PSRAM (4 MB) rather than SRAM (415 KB).
 * Mirrors the definition in nes/infones_core/InfoNES.h so this file can use
 * it without pulling in the whole InfoNES core header. */
#ifndef NES_PSRAM_BSS
#define NES_PSRAM_BSS __attribute__((section(".psram_noinit")))
#endif

#define DBG_TAG "FRAME"

/* ------------------------------------------------------------------ *
 *  Video: NES MJPEG frame when the emulator runs; otherwise a static
 *  software-encoded test pattern so the UVC pipe is alive (Phase 1).
 * ------------------------------------------------------------------ */
#define TEST_W  VIDEO_WIDTH
#define TEST_H  VIDEO_HEIGHT
/* These are only used by the Phase-1 test-pattern path. They are large
 * (120 KB + 64 KB at 256x240; up to ~491 KB + 256 KB at 512x480) so place
 * them in PSRAM (4 MB) instead of SRAM (415 KB). */
#if NES_FRAME_UPSCALE_2X
static uint16_t test_rgb565[512 * 480] NES_PSRAM_BSS;         /* 512x480 RGB565 = 491 KB */
static uint8_t  test_jpeg[256 * 1024] NES_PSRAM_BSS;         /* encoded MJPEG output     */
#else
static uint16_t test_rgb565[256 * 240] NES_PSRAM_BSS;         /* 256x240 RGB565 = 120 KB */
static uint8_t  test_jpeg[65536] NES_PSRAM_BSS;               /* encoded MJPEG output     */
#endif
static uint32_t test_jpeg_len = 0;
static bool     test_encoded   = false;

/* Simple color-bars / gradient test pattern in RGB565. */
static void build_test_pattern(void)
{
    for (int y = 0; y < TEST_H; y++) {
        for (int x = 0; x < TEST_W; x++) {
            uint8_t r = (uint8_t)((x * 255) / (TEST_W - 1));
            uint8_t g = (uint8_t)((y * 255) / (TEST_H - 1));
            uint8_t b = (uint8_t)(((x + y) * 255) / (TEST_W + TEST_H - 2));
            test_rgb565[y * TEST_W + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

int video_source_get_frame(const uint8_t **out_data, uint32_t *out_len)
{
    /* Phase 2: the NES emulator produces MJPEG frames. */
    if (nes_bridge_running()) {
        return nes_bridge_get_video(out_data, out_len);
    }

    /* Phase 1 fallback: encode a static test pattern once, then stream it
     * repeatedly. usbd_video_stream_start_write() copies the buffer into the
     * USB packet buf synchronously, so a plain SRAM buffer is safe here. */
    if (!test_encoded) {
        soft_mjpeg_init();
        build_test_pattern();
        test_jpeg_len = soft_mjpeg_encode(test_rgb565, TEST_W, TEST_H,
                                          90, test_jpeg, sizeof(test_jpeg));
        test_encoded = true;
        if (test_jpeg_len == 0) {
            LOG_I("[FRAME] soft_mjpeg_encode failed (test pattern)\r\n");
            return -1;
        }
        LOG_I("[FRAME] test MJPEG encoded, len=%u\r\n", (unsigned)test_jpeg_len);
    }

    *out_data = test_jpeg;
    *out_len  = test_jpeg_len;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Audio: NES APU ring, or a 1 kHz sine test tone fallback.
 * ------------------------------------------------------------------ */
#define TONE_HZ 1000
static const int16_t sine_lut[256] = {
    0, 196, 393, 589, 784, 979, 1174, 1368, 1561, 1753, 1944, 2134, 2322, 2509, 2695, 2879,
    3061, 3242, 3420, 3597, 3771, 3943, 4113, 4280, 4445, 4606, 4766, 4922, 5075, 5225, 5372, 5516,
    5657, 5794, 5928, 6058, 6184, 6307, 6426, 6541, 6652, 6759, 6862, 6961, 7055, 7146, 7232, 7314,
    7391, 7464, 7532, 7596, 7656, 7710, 7760, 7806, 7846, 7882, 7913, 7940, 7961, 7978, 7990, 7998,
    8000, 7998, 7990, 7978, 7961, 7940, 7913, 7882, 7846, 7806, 7760, 7710, 7656, 7596, 7532, 7464,
    7391, 7314, 7232, 7146, 7055, 6961, 6862, 6759, 6652, 6541, 6426, 6307, 6184, 6058, 5928, 5794,
    5657, 5516, 5372, 5225, 5075, 4922, 4766, 4606, 4445, 4280, 4113, 3943, 3771, 3597, 3420, 3242,
    3061, 2879, 2695, 2509, 2322, 2134, 1944, 1753, 1561, 1368, 1174, 979, 784, 589, 393, 196,
    0, -196, -393, -589, -784, -979, -1174, -1368, -1561, -1753, -1944, -2134, -2322, -2509, -2695, -2879,
    -3061, -3242, -3420, -3597, -3771, -3943, -4113, -4280, -4445, -4606, -4766, -4922, -5075, -5225, -5372, -5516,
    -5657, -5794, -5928, -6058, -6184, -6307, -6426, -6541, -6652, -6759, -6862, -6961, -7055, -7146, -7232, -7314,
    -7391, -7464, -7532, -7596, -7656, -7710, -7760, -7806, -7846, -7882, -7913, -7940, -7961, -7978, -7990, -7998,
    -8000, -7998, -7990, -7978, -7961, -7940, -7913, -7882, -7846, -7806, -7760, -7710, -7656, -7596, -7532, -7464,
    -7391, -7314, -7232, -7146, -7055, -6961, -6862, -6759, -6652, -6541, -6426, -6307, -6184, -6058, -5928, -5794,
    -5657, -5516, -5372, -5225, -5075, -4922, -4766, -4606, -4445, -4280, -4113, -3943, -3771, -3597, -3420, -3242,
    -3061, -2879, -2695, -2509, -2322, -2134, -1944, -1753, -1561, -1368, -1174, -979, -784, -589, -393, -196,
};
static uint32_t tone_idx = 0;

void audio_source_fill(uint8_t *buf, uint32_t bytes)
{
    uint32_t frames = bytes / (AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8));
    int16_t *pcm = (int16_t *)buf;

    if (nes_bridge_running()) {
        /* NES audio path: zero the buffer then fill from the ring
         * (underflow leaves the tail as silence). */
        memset(buf, 0, bytes);
        nes_bridge_get_audio(buf, bytes);
        return;
    }

    /* Phase 1 fallback: 1 kHz stereo sine test tone. */
    uint32_t step = (uint32_t)((uint64_t)TONE_HZ * 256 / AUDIO_SAMPLE_RATE);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = sine_lut[tone_idx & (256 - 1)];
        tone_idx += step;
        pcm[i * AUDIO_CHANNELS + 0] = s;
        pcm[i * AUDIO_CHANNELS + 1] = s;
    }
}
