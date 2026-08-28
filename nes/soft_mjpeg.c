/*
 * soft_mjpeg.c - Software baseline JPEG encoder (see soft_mjpeg.h).
 *
 * Algorithm validated bit-for-bit-equivalent against a Python oracle
 * (tools/soft_jpeg_oracle.py): round-trip via PIL gave max_abs_err<16.
 * Ported to fixed-point integer math for the FPU-less BL616.
 */
#include <string.h>
#include <stdint.h>

#include "soft_mjpeg.h"
#include "soft_mjpeg_tables.h"

/* ---------- zigzag (natural index -> zigzag position) ---------- */
static const uint8_t ZZ[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
static uint8_t ZZ_INV[64]; /* natural index -> zigzag position */

/* ---------- fixed-point DCT (S=64 cosine table) ---------- */
static const int16_t COS_TAB[8][8] = {
    {64, 64, 64, 64, 64, 64, 64, 64},
    {63, 53, 36, 12, -12, -36, -53, -63},
    {59, 24, -24, -59, -59, -24, 24, 59},
    {53, -12, -63, -36, 36, 63, 12, -53},
    {45, -45, -45, 45, 45, -45, -45, 45},
    {36, -63, 12, 53, -53, -12, 63, -36},
    {24, -59, 59, -24, -24, 59, -59, 24},
    {12, -36, 53, -63, 63, -53, 36, -12},
};
#define C0_7071 23170  /* round(0.70710678 * 32768) */

/* ---------- Huffman code tables (id 0 = Y, id 1 = Cb/Cr) ---------- */
static uint16_t dc_code[2][256];
static uint8_t  dc_len[2][256];
static uint16_t ac_code[2][256];
static uint8_t  ac_len[2][256];
static int huff_initialized = 0;

/* ---------- output buffer (header bytes) ---------- */
typedef struct {
    uint8_t *base;
    uint32_t pos;
    uint32_t cap;
} OutBuf;

static int out_byte(OutBuf *o, uint8_t b)
{
    if (o->pos >= o->cap) return -1;
    o->base[o->pos++] = b;
    return 0;
}

static int out_bytes(OutBuf *o, const uint8_t *p, uint32_t n)
{
    if (o->pos + n > o->cap) return -1;
    memcpy(o->base + o->pos, p, n);
    o->pos += n;
    return 0;
}

/* ---------- bit writer (MSB-first, 0xFF stuffing) ---------- */
typedef struct {
    uint8_t *buf;
    uint32_t len;
    uint32_t cap;
    uint32_t cur;
    int nbits;
} BitW;

static void bw_put(BitW *bw, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        uint8_t bit = (uint8_t)((val >> i) & 1);
        bw->cur = (bw->cur << 1) | bit;
        bw->nbits++;
        if (bw->nbits == 8) {
            if (bw->len >= bw->cap) return;
            bw->buf[bw->len++] = (uint8_t)bw->cur;
            if (bw->cur == 0xFF && bw->len < bw->cap)
                bw->buf[bw->len++] = 0x00; /* stuffing */
            bw->cur = 0;
            bw->nbits = 0;
        }
    }
}

static void bw_flush(BitW *bw)
{
    if (bw->nbits > 0) {
        bw->cur <<= (8 - bw->nbits);
        if (bw->len < bw->cap) {
            bw->buf[bw->len++] = (uint8_t)bw->cur;
            if (bw->cur == 0xFF && bw->len < bw->cap)
                bw->buf[bw->len++] = 0x00;
        }
        bw->cur = 0;
        bw->nbits = 0;
    }
}

/* ---------- Huffman parsing / code building ---------- */
static void build_codes(const uint8_t *bits, const uint8_t *vals, int nvals,
                        uint16_t *code, uint8_t *len)
{
    int codev = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++) {
            int sym = vals[k++];
            code[sym] = (uint16_t)codev;
            len[sym] = (uint8_t)l;
            codev++;
        }
        codev <<= 1;
    }
    (void)nvals;
}

void soft_mjpeg_init(void)
{
    if (huff_initialized) return;
    int i = 0;
    while (i + 4 < (int)JPEG_HUFFMAN_RAW_LEN) {
        if (JPEG_HUFFMAN_RAW[i] != 0xFF || JPEG_HUFFMAN_RAW[i + 1] != 0xC4) {
            i++;
            continue;
        }
        int length = (JPEG_HUFFMAN_RAW[i + 2] << 8) | JPEG_HUFFMAN_RAW[i + 3];
        int cls_id = JPEG_HUFFMAN_RAW[i + 4];
        int p = i + 5;
        uint8_t bits[17];
        for (int b = 0; b < 16; b++) bits[b + 1] = JPEG_HUFFMAN_RAW[p++];
        int nvals = 0;
        for (int b = 1; b <= 16; b++) nvals += bits[b];
        const uint8_t *vals = &JPEG_HUFFMAN_RAW[p];
        int id = cls_id & 0x0F;
        if (cls_id & 0x10)
            build_codes(bits, vals, nvals, ac_code[id], ac_len[id]);
        else
            build_codes(bits, vals, nvals, dc_code[id], dc_len[id]);
        i = i + 2 + length;
    }
    for (int k = 0; k < 64; k++) ZZ_INV[ZZ[k]] = (uint8_t)k;
    huff_initialized = 1;
}

/* ---------- quantisation ---------- */
static void qcalc(const uint8_t *in, int q, uint8_t *out)
{
    int num, den;
    if (q < 1)       { num = 5000; den = 100; }
    else if (q <= 50){ num = 5000; den = q * 100; }
    else if (q <= 100){ num = 200 - 2 * q; den = 100; }
    else             { num = 0;    den = 1; }
    for (int i = 0; i < 64; i++) {
        int r = in[i] * num;
        if (den != 1) r = (r + den / 2) / den;
        if (r > 255) out[i] = 255;
        else if (r < 1) out[i] = 1;
        else out[i] = (uint8_t)r;
    }
}

/* ---------- DCT + per-block entropy ---------- */
static void fdct_int(const int16_t *blk, int16_t *out)
{
    int32_t A[64], B[64];
    for (int y = 0; y < 8; y++) {
        for (int u = 0; u < 8; u++) {
            int32_t s = 0;
            for (int n = 0; n < 8; n++) s += (int32_t)blk[y * 8 + n] * COS_TAB[u][n];
            A[u * 8 + y] = s;
        }
    }
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            int32_t s = 0;
            for (int n = 0; n < 8; n++) s += A[u * 8 + n] * COS_TAB[v][n];
            B[u * 8 + v] = s;
        }
    }
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            int64_t t = B[u * 8 + v];
            if (u == 0) t = (t * C0_7071) >> 15;
            if (v == 0) t = (t * C0_7071) >> 15;
            out[v * 8 + u] = (int16_t)(t >> 14);
        }
    }
}

static void encode_block(BitW *bw, const int16_t *qz, /* zigzag ordered */
                         const uint16_t *dc_c, const uint8_t *dc_l,
                         const uint16_t *ac_c, const uint8_t *ac_l,
                         int *prev_dc)
{
    int dc = qz[0];
    int diff = dc - *prev_dc;
    int size = 0;
    {   /* size = bit length of |diff| (portable, no FPU/clz dependency) */
        int v = diff; if (v < 0) v = -v;
        while (v) { size++; v >>= 1; }
    }
    bw_put(bw, dc_c[size], dc_l[size]);
    if (size) {
        if (diff > 0) bw_put(bw, (uint32_t)diff, size);
        else bw_put(bw, (uint32_t)(((-diff) ^ ((1 << size) - 1)) & ((1 << size) - 1)), size);
    }
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int val = qz[k];
        if (val == 0) {
            run++;
            if (run == 16) {
                bw_put(bw, ac_c[0xF0], ac_l[0xF0]);
                run = 0;
            }
            continue;
        }
        while (run >= 16) {
            bw_put(bw, ac_c[0xF0], ac_l[0xF0]);
            run -= 16;
        }
        int mag = val; if (mag < 0) mag = -mag;
        int asize = 0; { int t = mag; while (t) { asize++; t >>= 1; } }
        int rs = (run << 4) | asize;
        bw_put(bw, ac_c[rs], ac_l[rs]);
        if (val > 0) bw_put(bw, (uint32_t)val, asize);
        else bw_put(bw, (uint32_t)(((mag) ^ ((1 << asize) - 1)) & ((1 << asize) - 1)), asize);
        run = 0;
    }
    if (run > 0) bw_put(bw, ac_c[0x00], ac_l[0x00]); /* EOB */
    *prev_dc = dc;
}

/* ---------- RGB565 -> YCbCr (BT.601 studio/limited range, integer) ----------
 * UVC MJPEG is consumed by the host as a VIDEO stream: Windows usbvideo.sys
 * decodes it and exposes YUY2 in BT.601 LIMITED range (Y in [16,235],
 * Cb/Cr in [16,240]). Encoding full-range YCbCr here would make the host
 * apply the 16-235 expansion to already-full-range data, crushing darks and
 * shifting low/mid-luma colors (the "colors look off / shifted" symptom).
 * We therefore emit limited-range YCbCr to match the host pipeline.
 * Coefficients x256: Y=16+(66,129,25), Cb=128+(-38,-74,112), Cr=128+(112,-94,-18). */
static inline void rgb565_ycbcr(uint16_t p, int *y, int *cb, int *cr)
{
    int r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2);
    int g = (p >> 5) & 0x3F; g = (g << 2) | (g >> 4);
    int b = p & 0x1F; b = (b << 3) | (b >> 2);
    *y  = 16 + ((66 * r + 129 * g + 25 * b) >> 8);
    *cb = 128 + ((-38 * r - 74 * g + 112 * b) >> 8);
    *cr = 128 + ((112 * r - 94 * g - 18 * b) >> 8);
}

/* Fill one 8x8 block. comp: 0=Y,1=Cb,2=Cr. sub_h: horizontal subsample
 * factor (1 for luma / 2 for chroma in 4:2:2).
 *
 * For 4:2:2 chroma we average the Cb/Cr of two adjacent horizontal pixels
 * (centered sampling). Windows usbvideo.sys / camera app upsamples 4:2:2
 * back to 4:4:4 assuming centered chroma; left/cosited sampling produces a
 * half-pixel chroma shift that shows up as red/green edge fringing. */
static void fill_block(const uint16_t *rgb565, int w, int h,
                       int bx, int by, int comp, int sub_h, int16_t *blk)
{
    for (int yy = 0; yy < 8; yy++) {
        int y = by + yy;
        if (y >= h) y = h - 1;
        for (int xx = 0; xx < 8; xx++) {
            int s;
            if (sub_h == 1) {
                int x = bx + xx;
                if (x >= w) x = w - 1;
                int Y, Cb, Cr;
                rgb565_ycbcr(rgb565[y * w + x], &Y, &Cb, &Cr);
                s = (comp == 0) ? Y : (comp == 1) ? Cb : Cr;
            } else {
                /* centered chroma: average two adjacent pixels */
                int x0 = bx + xx * 2;
                int x1 = x0 + 1;
                if (x0 >= w) x0 = w - 1;
                if (x1 >= w) x1 = w - 1;
                int Y0, Cb0, Cr0, Y1, Cb1, Cr1;
                rgb565_ycbcr(rgb565[y * w + x0], &Y0, &Cb0, &Cr0);
                rgb565_ycbcr(rgb565[y * w + x1], &Y1, &Cb1, &Cr1);
                if (comp == 1)      s = (Cb0 + Cb1) >> 1;
                else if (comp == 2) s = (Cr0 + Cr1) >> 1;
                else                s = (Y0 + Y1) >> 1;
            }
            blk[yy * 8 + xx] = (int16_t)(s - 128);
        }
    }
}

static void quantize_block(const int16_t *coeff, const uint8_t *qt, int16_t *qz)
{
    for (int k = 0; k < 64; k++) {
        int v = coeff[k];
        int qv = (v >= 0) ? (v + (qt[k] >> 1)) / qt[k]
                          : (v - (qt[k] >> 1)) / qt[k];
        qz[ZZ_INV[k]] = (int16_t)qv;
    }
}

/* ---------- public API ---------- */
uint32_t soft_mjpeg_encode(const uint16_t *rgb565, int w, int h,
                           int quality, uint8_t *out, uint32_t out_size)
{
    soft_mjpeg_init();

    uint8_t qt_y[64], qt_uv[64];
    qcalc(JPEG_QY, quality, qt_y);
    qcalc(JPEG_QUV, quality, qt_uv);

    OutBuf ob;
    ob.base = out; ob.pos = 0; ob.cap = out_size;

    /* SOI */
    out_byte(&ob, 0xFF); out_byte(&ob, 0xD8);
    /* DQT x2 */
    out_byte(&ob, 0xFF); out_byte(&ob, 0xDB); out_byte(&ob, 0x00); out_byte(&ob, 0x43);
    out_byte(&ob, 0x00); out_bytes(&ob, qt_y, 64);
    out_byte(&ob, 0xFF); out_byte(&ob, 0xDB); out_byte(&ob, 0x00); out_byte(&ob, 0x43);
    out_byte(&ob, 0x01); out_bytes(&ob, qt_uv, 64);
    /* SOF0 (4:2:2): Y full-res, Cb/Cr horizontal 2:1. Native 4:2:2 lets the
     * host present YUY2 directly without re-subsampling our 4:4:4 stream,
     * which is what caused the red/green chroma-misalignment on Windows
     * usbvideo.sys (its 4:4:4->4:2:2 conversion mis-positions chroma). */
    out_byte(&ob, 0xFF); out_byte(&ob, 0xC0); out_byte(&ob, 0x00); out_byte(&ob, 0x11);
    out_byte(&ob, 0x08);
    out_byte(&ob, (uint8_t)((h >> 8) & 0xFF)); out_byte(&ob, (uint8_t)(h & 0xFF));
    out_byte(&ob, (uint8_t)((w >> 8) & 0xFF)); out_byte(&ob, (uint8_t)(w & 0xFF));
    out_byte(&ob, 0x03);
    out_byte(&ob, 0x01); out_byte(&ob, 0x21); out_byte(&ob, 0x00); /* Y  : samp(2,1) qt0 */
    out_byte(&ob, 0x02); out_byte(&ob, 0x11); out_byte(&ob, 0x01); /* Cb : samp(1,1) qt1 */
    out_byte(&ob, 0x03); out_byte(&ob, 0x11); out_byte(&ob, 0x01); /* Cr : samp(1,1) qt1 */
    /* DHT (verbatim from HW path) */
    out_bytes(&ob, JPEG_HUFFMAN_RAW, JPEG_HUFFMAN_RAW_LEN);
    /* SOS (4:2:2) */
    out_byte(&ob, 0xFF); out_byte(&ob, 0xDA); out_byte(&ob, 0x00); out_byte(&ob, 0x0C);
    out_byte(&ob, 0x03);
    out_byte(&ob, 0x01); out_byte(&ob, 0x00); /* Y  -> DC/AC id 0 */
    out_byte(&ob, 0x02); out_byte(&ob, 0x11); /* Cb -> DC/AC id 1 */
    out_byte(&ob, 0x03); out_byte(&ob, 0x11); /* Cr -> DC/AC id 1 */
    out_byte(&ob, 0x00); out_byte(&ob, 0x3F); out_byte(&ob, 0x00);

    /* scan (MCU 16x8: 2x Y + 1x Cb + 1x Cr; chroma cosited at even column) */
    BitW bw;
    bw.buf = out + ob.pos;
    bw.len = 0;
    bw.cap = out_size - ob.pos;
    bw.cur = 0; bw.nbits = 0;

    int prev_dc[3] = {0, 0, 0};
    int16_t blk[64];
    int16_t coeff[64];
    int16_t qz[64];

    for (int by = 0; by < h; by += 8) {
        for (int bx = 0; bx < w; bx += 16) {
            /* Y block 0 */
            fill_block(rgb565, w, h, bx,     by, 0, 1, blk);
            fdct_int(blk, coeff);
            quantize_block(coeff, qt_y, qz);
            encode_block(&bw, qz, dc_code[0], dc_len[0], ac_code[0], ac_len[0], &prev_dc[0]);
            /* Y block 1 */
            fill_block(rgb565, w, h, bx + 8, by, 0, 1, blk);
            fdct_int(blk, coeff);
            quantize_block(coeff, qt_y, qz);
            encode_block(&bw, qz, dc_code[0], dc_len[0], ac_code[0], ac_len[0], &prev_dc[0]);
            /* Cb (horizontal 2:1, cosited even column) */
            fill_block(rgb565, w, h, bx, by, 1, 2, blk);
            fdct_int(blk, coeff);
            quantize_block(coeff, qt_uv, qz);
            encode_block(&bw, qz, dc_code[1], dc_len[1], ac_code[1], ac_len[1], &prev_dc[1]);
            /* Cr */
            fill_block(rgb565, w, h, bx, by, 2, 2, blk);
            fdct_int(blk, coeff);
            quantize_block(coeff, qt_uv, qz);
            encode_block(&bw, qz, dc_code[1], dc_len[1], ac_code[1], ac_len[1], &prev_dc[2]);
        }
    }
    bw_flush(&bw);
    ob.pos += bw.len;

    /* EOI */
    out_byte(&ob, 0xFF); out_byte(&ob, 0xD9);

    return ob.pos;
}
