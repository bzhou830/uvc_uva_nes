#ifndef __HW_MJPEG_H__
#define __HW_MJPEG_H__

#include <stdint.h>

/*
 * Hardware JPEG (MJPEG) encoder wrapper for BL616.
 *
 * BL616 has a dedicated MJPEG encoder block. It ingests a YUV422 (YUYV)
 * memory buffer and emits a baseline JPEG stream (hardware does the DCT /
 * quantise / Huffman). This is orders of magnitude faster than the previous
 * software fdct encoder and frees enough CPU to run the NES emulator at full
 * frame rate even at 512x480 (2x upscale).
 *
 * Encoder is configured once with hw_mjpeg_init(); each frame is produced by
 * hw_mjpeg_encode(), which converts the NES RGB565 frame to YUYV, kicks the
 * hardware, busy-waits for completion, and returns the JPEG length. The
 * caller is expected to feed a fresh RGB565 buffer each call.
 */

/* Configure the hardware encoder for a fixed (w x h) resolution.
 * Must be called before the first hw_mjpeg_encode(). */
void hw_mjpeg_init(int w, int h, uint8_t quality);

/* Encode one RGB565 frame to MJPEG. Returns the JPEG byte length, or 0 on
 * failure. Output is written to 'out' (caller-supplied, >= out_size). */
uint32_t hw_mjpeg_encode(const uint16_t *rgb565, int w, int h,
                         uint8_t quality, uint8_t *out, uint32_t out_size);

#endif /* __HW_MJPEG_H__ */
