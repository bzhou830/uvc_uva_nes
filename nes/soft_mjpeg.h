/*
 * soft_mjpeg.h - Software baseline JPEG (MJPEG) encoder for BL616 (no HW MJPEG).
 *
 * Baseline JPEG, 4:4:4 YCbCr, fixed-point integer DCT (no FPU required).
 * Quant + Huffman tables are byte-identical to the BL618 HW MJPEG path
 * (see soft_mjpeg_tables.h, generated from jpeg_head.c) so the output is
 * compatible with the existing UVC descriptor / viewers.
 *
 * Input: RGB565 framebuffer (row-major). Output: JPEG byte stream.
 */
#ifndef _SOFT_MJPEG_H_
#define _SOFT_MJPEG_H_

#include <stdint.h>

/* Initialise Huffman code tables (idempotent). */
void soft_mjpeg_init(void);

/* Encode one RGB565 frame into a JPEG.
 *   rgb565 : w*h pixels, row-major, RGB565 little-endian
 *   w, h   : dimensions (multiple of 8 recommended)
 *   quality: 1..100
 *   out    : caller-provided buffer of out_size bytes
 * Returns JPEG length (>0) or 0 on error/overflow. */
uint32_t soft_mjpeg_encode(const uint16_t *rgb565, int w, int h,
                           int quality, uint8_t *out, uint32_t out_size);

#endif /* _SOFT_MJPEG_H_ */
