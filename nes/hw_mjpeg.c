/*
 * hw_mjpeg.c - BL616 hardware MJPEG encoder wrapper.
 *
 * Replaces the software fdct encoder (soft_mjpeg.c) for NES frames. The
 * BL616 MJPEG block takes a YUV422_YUYV memory buffer and produces a
 * baseline JPEG stream in hardware. Flow per frame:
 *
 *   1. convert NES RGB565 -> YUYV (BT.601 limited range, software, cheap)
 *   2. dcache_clean the YUYV buffer (PSRAM is cacheable; HW DMAs from it)
 *   3. bflb_mjpeg_update_input_output_buff() + bflb_mjpeg_sw_run(1)
 *   4. busy-wait MJPEG_INTSTS_ONE_FRAME
 *   5. bflb_mjpeg_get_frame_info() -> JPEG length, then invalidate dcache
 *
 * Chroma is packed centered (average of two adjacent pixels) into the YUYV
 * stream. When fed a 2x-upscaled frame (512x480) the two adjacent output
 * pixels are copies of the same NES pixel, so the chroma lands exactly on
 * the NES pixel and aligns with the host's cosited 4:2:2 decode (no
 * red/green fringing).
 */
#include <string.h>

#include "bflb_core.h"
#include "bflb_mjpeg.h"
#include "bflb_l1c.h"

#include "jpeg_head.h"
#include "uvc_uac.h"
#include "hw_mjpeg.h"
#include "log.h"

/* --- profiling: RISC-V mcycle/mcycleh (CPU 320MHz) --- */
static inline uint64_t prof_now(void) {
    uint32_t lo, hi;
    asm volatile("csrr %0, mcycle\n csrr %1, mcycleh" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t g_pre_us = 0, g_enc_us = 0;
static uint32_t g_prof_n = 0;

#ifndef NES_PSRAM_BSS
#define NES_PSRAM_BSS __attribute__((section(".psram_noinit")))
#endif

static struct bflb_device_s *mjpeg_dev = NULL;

/* YUYV input buffer: 2 bytes/pixel. Sized for the 2x (512x480) path; when
 * the 2x switch is off only 256x240 is used. 32-byte aligned for DMA. */
#if NES_FRAME_UPSCALE_2X
#define HW_MJPEG_W  512
#define HW_MJPEG_H  480
#else
#define HW_MJPEG_W  256
#define HW_MJPEG_H  240
#endif
static uint8_t yuyv_buf[HW_MJPEG_W * HW_MJPEG_H * 2] NES_PSRAM_BSS __ALIGNED(32);

/* RGB565 -> YCbCr (BT.601 studio/limited range, integer).
 * Matches the colour pipeline the previous software encoder used so the
 * on-screen colours are unchanged. */
static inline void rgb565_ycbcr(uint16_t p, int *y, int *cb, int *cr)
{
    int r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);
    int g = (p >> 5) & 0x3F; g = (g << 2) | (g >> 4);
    int b = p & 0x1F; b = (b << 3) | (b >> 2);
    *y  = 16 + ((66 * r + 129 * g + 25 * b) >> 8);
    *cb = 128 + ((-38 * r - 74 * g + 112 * b) >> 8);
    *cr = 128 + ((112 * r - 94 * g - 18 * b) >> 8);
}

void hw_mjpeg_init(int w, int h, uint8_t quality)
{
    uint8_t jpg_head_buf[800];
    uint32_t jpg_head_len;

    mjpeg_dev = bflb_device_get_by_name("mjpeg");
    if (mjpeg_dev == NULL) {
        return;   /* device table has no mjpeg; encoding will fail gracefully */
    }

    struct bflb_mjpeg_config_s cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.format        = MJPEG_FORMAT_YUV422_YUYV;
    cfg.quality       = quality;
    cfg.rows          = (uint16_t)h;
    cfg.resolution_x  = (uint16_t)w;
    cfg.resolution_y  = (uint16_t)h;
    cfg.input_bufaddr0 = (uint32_t)yuyv_buf;
    cfg.input_bufaddr1 = 0;
    cfg.output_bufaddr = 0;
    cfg.output_bufsize = 0;
    cfg.input_yy_table = NULL;   /* use hardware default quant tables */
    cfg.input_uv_table = NULL;

    bflb_mjpeg_init(mjpeg_dev, &cfg);

    /* Build the JPEG header (DQT/SOF/SOS) and let the hardware append it +
     * the EOI tail automatically. YUV_MODE_422 => 4:2:2, matching the
     * YUYV input format. */
    jpg_head_len = JpegHeadCreate(YUV_MODE_422, quality, w, h, jpg_head_buf);
    bflb_mjpeg_fill_jpeg_header_tail(mjpeg_dev, jpg_head_buf, jpg_head_len);

    /* Enable the one-frame-done interrupt (we poll the status bit instead of
     * installing an ISR, so just unmask it). */
    bflb_mjpeg_tcint_mask(mjpeg_dev, false);
}

uint32_t hw_mjpeg_encode(const uint16_t *rgb565, int w, int h,
                         uint8_t quality, uint8_t *out, uint32_t out_size)
{
    if (mjpeg_dev == NULL) {
        return 0;
    }

    /* The hardware encodes at (HW_MJPEG_W x HW_MJPEG_H). When 2x upscaling
     * is enabled the input is NES-native (w x h) and we expand each NES
     * pixel into a 2x2 YUYV block on the fly (no extra RGB565 scratch). */
    int ow = HW_MJPEG_W;
    int oh = HW_MJPEG_H;
    int scale = (w * 2 == ow && h * 2 == oh) ? 2 : 1;
    uint8_t *yuyv = yuyv_buf;
    uint32_t yuyv_sz = (uint32_t)ow * (uint32_t)oh * 2;
    uint64_t t0 = prof_now();

    /* 1. RGB565 -> YUYV.
     *   - 2x upscale (256x240 -> 512x480): each NES pixel becomes a 2x2
     *     YUYV block. The two output pixels of a pair are copies of the
     *     same NES pixel, so centered chroma averaging reduces to the
     *     pixel's own chroma. Convert once per NES pixel (4x fewer
     *     rgb565_ycbcr calls than the naive per-output-pixel loop) and
     *     replicate into the 2x2 block.
     *   - 1x (native 256x240): centered chroma across the two adjacent
     *     NES pixels, the standard 4:2:2 packing. */
    if (scale == 2) {
        for (int ny = 0; ny < h; ny++) {
            const uint16_t *row = rgb565 + (size_t)ny * w;
            uint8_t *o0 = yuyv + (size_t)(ny * 2)     * ow * 2;  /* even row */
            uint8_t *o1 = yuyv + (size_t)(ny * 2 + 1) * ow * 2;  /* odd row  */
            for (int nx = 0; nx < w; nx++) {
                int Y, Cb, Cr;
                rgb565_ycbcr(row[nx], &Y, &Cb, &Cr);
                int k = nx * 4;            /* 2 output px * 2 bytes */
                o0[k]     = (uint8_t)Y; o0[k + 1] = (uint8_t)Cb;
                o0[k + 2] = (uint8_t)Y; o0[k + 3] = (uint8_t)Cr;
                o1[k]     = (uint8_t)Y; o1[k + 1] = (uint8_t)Cb;
                o1[k + 2] = (uint8_t)Y; o1[k + 3] = (uint8_t)Cr;
            }
        }
    } else {
        for (int y = 0; y < oh; y++) {
            const uint16_t *row = rgb565 + (size_t)y * w;
            uint8_t *o = yuyv + (size_t)y * ow * 2;
            for (int x = 0; x < ow; x += 2) {
                int sx1 = (x + 1 < ow) ? x + 1 : x;
                int Y0, Cb0, Cr0, Y1, Cb1, Cr1;
                rgb565_ycbcr(row[x],  &Y0, &Cb0, &Cr0);
                rgb565_ycbcr(row[sx1], &Y1, &Cb1, &Cr1);
                o[0] = (uint8_t)Y0;
                o[1] = (uint8_t)((Cb0 + Cb1) >> 1);
                o[2] = (uint8_t)Y1;
                o[3] = (uint8_t)((Cr0 + Cr1) >> 1);
                o += 4;
            }
        }
    }

    /* 2. Make sure the HW sees the freshly written YUYV pixels. */
    bflb_l1c_dcache_clean_range(yuyv, yuyv_sz);
    uint64_t t1 = prof_now();
    g_pre_us += (t1 - t0);

    /* 3. Kick the encoder. */
    bflb_mjpeg_update_input_output_buff(mjpeg_dev, yuyv, NULL, out, out_size);
    bflb_mjpeg_sw_run(mjpeg_dev, 1);

    /* 4. Wait for completion (single-core, no other work here). */
    while (!(bflb_mjpeg_get_intstatus(mjpeg_dev) & MJPEG_INTSTS_ONE_FRAME)) {
        /* busy poll */
    }
    bflb_mjpeg_int_clear(mjpeg_dev, MJPEG_INTCLR_ONE_FRAME);
    uint64_t t2 = prof_now();
    g_enc_us += (t2 - t1);
    if (++g_prof_n >= 60) {
        LOG_I("[PROF] pre=%luus enc=%luus (CPU 320MHz)\r\n",
              (unsigned long)(g_pre_us / g_prof_n / 320),
              (unsigned long)(g_enc_us / g_prof_n / 320));
        g_pre_us = 0; g_enc_us = 0; g_prof_n = 0;
    }

    /* 5. Retrieve the encoded stream. */
    uint8_t *pic = NULL;
    uint32_t len = bflb_mjpeg_get_frame_info(mjpeg_dev, &pic);
    bflb_mjpeg_pop_one_frame(mjpeg_dev);
    bflb_mjpeg_stop(mjpeg_dev);

    /* The encoder wrote into 'out'; make sure the CPU sees it. */
    bflb_l1c_dcache_invalidate_range(out, len);

    if (len == 0 || len > out_size) {
        return 0;
    }
    return len;
}
