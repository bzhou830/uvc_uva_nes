#!/usr/bin/env python3
# soft_jpeg_oracle.py
# Reference (float) baseline JPEG encoder used to DE-RISK the algorithm before
# porting it to C for the BL616 software-MJPEG path.
#
# It reuses the EXACT quant + Huffman tables emitted by the BL618 hardware
# MJPEG path (jpeg_head.c) so the produced MJPEG is byte-compatible with the
# existing UVC descriptor. Tables are parsed straight out of jpeg_head.c to
# avoid any transcription error.
#
# Output: 4:4:4 YCbCr baseline JPEG (sampling 1x1,1x1,1x1). The HW path uses
# 4:2:2; 4:4:4 is simpler to implement correctly and still decodes fine in
# every UVC viewer. We can drop to 4:2:2 later as a bandwidth optimization.

import math
import re
import sys

JPEG_HEAD_C = r"D:/bouffalo_sdk/examples/cherryusb/uvc_uac_nes/../uvc_uac_nes"  # placeholder


def load_tables(path):
    """Parse tableQy, tableQuv, tableHuffman out of jpeg_head.c."""
    with open(path, "r") as f:
        txt = f.read()

    def hex_array(name):
        m = re.search(name + r"\[\]\s*=\s*\{([^}]*)\}", txt, re.S)
        assert m, "cannot find " + name
        # tableQy/tableQuv are decimal; tableHuffman is 0x hex
        toks = re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))
        return [int(t, 16) if t.lower().startswith("0x") else int(t) for t in toks]

    qy = hex_array(r"tableQy")
    quv = hex_array(r"tableQuv")
    huff = hex_array(r"tableHuffman")
    assert len(qy) == 64 and len(quv) == 64, (len(qy), len(quv))
    return qy, quv, huff


def qcalc(in_tbl, q):
    if q < 1:
        tq = 5000.0
    elif q <= 50:
        tq = 5000.0 / q
    elif q <= 100:
        tq = 200.0 - 2.0 * q
    else:
        tq = 0.0
    tq = tq / 100.0
    out = []
    for v in in_tbl:
        r = v * tq
        if r > 255.0:
            out.append(255)
        elif r < 1.0:
            out.append(1)
        else:
            out.append(int(r + 0.5))
    return out


def parse_dht(huff_bytes):
    """Yield (class_id, bits[1..16], values[]) for each FFC4 segment."""
    i = 0
    segs = []
    while i + 4 < len(huff_bytes):
        if huff_bytes[i] != 0xFF or huff_bytes[i + 1] != 0xC4:
            i += 1
            continue
        length = (huff_bytes[i + 2] << 8) | huff_bytes[i + 3]
        cls_id = huff_bytes[i + 4]
        p = i + 5
        bits = [0] + list(huff_bytes[p:p + 16])
        p += 16
        nval = sum(bits[1:])
        values = list(huff_bytes[p:p + nval])
        p += nval
        segs.append((cls_id, bits, values))
        i = i + 2 + length  # skip marker + length + payload
    return segs


def build_codes(bits, values):
    code = 0
    codes = {}
    k = 0
    for l in range(1, 17):
        for _ in range(bits[l]):
            sym = values[k]
            k += 1
            codes[sym] = (code, l)
            code += 1
        code <<= 1
    return codes


# ---- DCT ----
def fdct(block8x8):
    """block8x8: flat list of 64 (already level-shifted). Returns 64 coeffs."""
    out = [0.0] * 64
    for u in range(8):
        for v in range(8):
            s = 0.0
            cu = math.sqrt(0.5) if u == 0 else 1.0
            cv = math.sqrt(0.5) if v == 0 else 1.0
            for x in range(8):
                for y in range(8):
                    s += (cu * cv * block8x8[y * 8 + x] *
                          math.cos((2 * x + 1) * u * math.pi / 16.0) *
                          math.cos((2 * y + 1) * v * math.pi / 16.0))
            out[v * 8 + u] = s / 4.0
    return out


ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33,
      40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50,
      43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46,
      53, 60, 61, 54, 47, 55, 62, 63]


# ---- Fixed-point integer DCT (for porting to C / no-FPU BL616) ----
import math as _math
_S = 64
_COS = [[round(_math.cos((2 * n + 1) * u * _math.pi / 16.0) * _S) for n in range(8)] for u in range(8)]
_C0 = 23170  # round(0.70710678 * 32768)


def fdct_int(block8x8):
    A = [[0] * 8 for _ in range(8)]
    B = [[0] * 8 for _ in range(8)]
    for y in range(8):
        for u in range(8):
            s = 0
            for n in range(8):
                s += block8x8[y * 8 + n] * _COS[u][n]
            A[u][y] = s
    for u in range(8):
        for v in range(8):
            s = 0
            for n in range(8):
                s += A[u][n] * _COS[v][n]
            B[u][v] = s
    out = [0] * 64
    for u in range(8):
        for v in range(8):
            t = B[u][v]
            if u == 0:
                t = (t * _C0) >> 15
            if v == 0:
                t = (t * _C0) >> 15
            out[v * 8 + u] = t >> 14  # / (4 * S^2) = /16384
    return out



class BitWriter:
    def __init__(self):
        self.bytes = bytearray()
        self.cur = 0
        self.nbits = 0

    def write(self, val, n):
        for i in range(n - 1, -1, -1):
            b = (val >> i) & 1
            self.cur = (self.cur << 1) | b
            self.nbits += 1
            if self.nbits == 8:
                self._emit(self.cur)
                self.cur = 0
                self.nbits = 0

    def _emit(self, b):
        self.bytes.append(b)
        if b == 0xFF:
            self.bytes.append(0x00)  # byte stuffing

    def flush(self):
        if self.nbits > 0:
            self.cur = self.cur << (8 - self.nbits)
            self._emit(self.cur)
            self.cur = 0
            self.nbits = 0


def encode_block(wr, coeffs_zig, dc_table, ac_table, prev_dc):
    """coeffs_zig: 64 quantized coeffs in zigzag order."""
    dc = coeffs_zig[0]
    diff = dc - prev_dc
    # DC
    if diff == 0:
        size = 0
    else:
        size = diff.bit_length()
    wr.write(dc_table[size][0], dc_table[size][1])
    if size:
        if diff > 0:
            wr.write(diff, size)
        else:
            wr.write(((-diff) ^ ((1 << size) - 1)) & ((1 << size) - 1), size)
    # AC
    run = 0
    for k in range(1, 64):
        v = coeffs_zig[k]
        if v == 0:
            run += 1
            if run == 16:
                wr.write(ac_table[0xF0][0], ac_table[0xF0][1])  # ZRL
                run = 0
            continue
        while run >= 16:
            wr.write(ac_table[0xF0][0], ac_table[0xF0][1])
            run -= 16
        mag = abs(v)
        size = mag.bit_length()
        rs = (run << 4) | size
        wr.write(ac_table[rs][0], ac_table[rs][1])
        if v > 0:
            wr.write(v, size)
        else:
            wr.write(((-v) ^ ((1 << size) - 1)) & ((1 << size) - 1), size)
        run = 0
    if run > 0:
        wr.write(ac_table[0x00][0], ac_table[0x00][1])  # EOB
    return dc


def encode_jpeg(rgb, w, h, quality, qy, quv, dht_segs, use_int=False):
    # build tables
    qy_s = qcalc(qy, quality)
    quv_s = qcalc(quv, quality)
    dc_codes = {}
    ac_codes = {}
    for cls_id, bits, values in dht_segs:
        codes = build_codes(bits, values)
        if cls_id & 0x10:
            ac_codes[cls_id & 0x0F] = codes
        else:
            dc_codes[cls_id & 0x0F] = codes

    # forward DCT needs both Y and C tables; component 0=Y uses qy_s, 1/2=CbCr use quv_s
    quant_for = {0: qy_s, 1: quv_s, 2: quv_s}

    # RGB -> YCbCr
    Y = [0] * (w * h)
    Cb = [0] * (w * h)
    Cr = [0] * (w * h)
    for i in range(w * h):
        r = rgb[i * 3]; g = rgb[i * 3 + 1]; b = rgb[i * 3 + 2]
        Y[i] = int(0.299 * r + 0.587 * g + 0.114 * b)
        Cb[i] = int(-0.168736 * r - 0.331264 * g + 0.5 * b + 128)
        Cr[i] = int(0.5 * r - 0.418688 * g - 0.081312 * b + 128)

    comps = [Y, Cb, Cr]
    wr = BitWriter()
    prev_dc = [0, 0, 0]

    # Interleaved 4:4:4: one MCU = (Y block, Cb block, Cr block), raster order.
    inv = [0] * 64
    for i in range(64):
        inv[ZZ[i]] = i  # natural index -> zigzag position
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for c in range(3):
                q = quant_for[c]
                data = comps[c]
                dc_t = dc_codes[0 if c == 0 else 1]
                ac_t = ac_codes[0 if c == 0 else 1]
                blk = [0] * 64
                for yy in range(8):
                    for xx in range(8):
                        x = min(bx + xx, w - 1)
                        y = min(by + yy, h - 1)
                        blk[yy * 8 + xx] = data[y * w + x] - 128
                coeff = fdct_int(blk) if use_int else fdct(blk)
                qz = [0] * 64
                for k in range(64):
                    qz[inv[k]] = int(round(coeff[k] / q[k]))
                prev_dc[c] = encode_block(wr, qz, dc_t, ac_t, prev_dc[c])

    wr.flush()
    return bytes(wr.bytes)


def build_jpeg(rgb, w, h, quality, jpeg_head_c_path, use_int=False):
    qy, quv, huff = load_tables(jpeg_head_c_path)
    dht_segs = parse_dht(huff)
    # Emit header pipeline identical (except 4:4:4 sampling) to jpeg_head.c
    head = bytearray()
    head += b"\xFF\xD8"
    # DQT x2
    for tid, tbl in ((0, qy), (1, quv)):
        q = qcalc(tbl, quality)
        head += b"\xFF\xDB\x00\x43" + bytes([tid]) + bytes(q)
    # SOF0 (4:4:4): 3 components, sampling 1x1 each
    sof = bytearray(b"\xFF\xC0\x00\x11\x08")
    sof += bytes([(h >> 8) & 0xFF, h & 0xFF, (w >> 8) & 0xFF, w & 0xFF])
    sof += b"\x03"  # 3 components
    sof += b"\x01\x11\x00"  # Y  id1 sampling 0x11 quant 0
    sof += b"\x02\x11\x01"  # Cb id2 sampling 0x11 quant 1
    sof += b"\x03\x11\x01"  # Cr id3 sampling 0x11 quant 1
    head += sof
    # DHT (verbatim from tableHuffman)
    head += bytes(huff)
    # SOS (4:4:4)
    sos = bytearray(b"\xFF\xDA\x00\x0C\x03")
    sos += b"\x01\x00"  # Y  -> DC tbl 0, AC tbl 0
    sos += b"\x02\x11"  # Cb -> DC tbl 1, AC tbl 1
    sos += b"\x03\x11"  # Cr -> DC tbl 1, AC tbl 1
    sos += b"\x00\x3F\x00"
    head += sos

    scan = encode_jpeg(rgb, w, h, quality, qy, quv, dht_segs, use_int)
    return bytes(head) + scan + b"\xFF\xD9"


if __name__ == "__main__":
    import os
    from PIL import Image
    import numpy as np

    # locate jpeg_head.c next to this file's grandparent (uvc_uac_nes/main/..)
    here = os.path.dirname(os.path.abspath(__file__))
    # The reference jpeg_head.c lives in the nofrendo port; allow override via argv[1]
    jhc = sys.argv[2] if len(sys.argv) > 2 else \
        r"C:/Users/lx/WorkBuddy/2026-07-22-11-23-28/bl618-nofrendo/main/jpeg_head.c"
    out = sys.argv[1] if len(sys.argv) > 1 else "oracle_test.jpg"

    # Build a synthetic 64x64 test image (RGB gradient + color bars)
    W = H = 64
    arr = np.zeros((H, W, 3), dtype=np.uint8)
    for y in range(H):
        for x in range(W):
            arr[y, x, 0] = int(255 * x / (W - 1))
            arr[y, x, 1] = int(255 * y / (H - 1))
            arr[y, x, 2] = int(128 + 127 * math.sin(2 * math.pi * x / W))

    rgb = arr.flatten().tolist()
    jpeg = build_jpeg(rgb, W, H, 50, jhc, use_int=False)
    with open(out, "wb") as f:
        f.write(jpeg)
    print("wrote", out, len(jpeg), "bytes (float DCT)")

    # Round-trip correctness check via PIL
    dec = np.asarray(Image.open(out).convert("RGB"))
    diff = np.abs(dec.astype(int) - arr.astype(int))
    maxd = int(diff.max())
    mean = float(diff.mean())
    print(f"decode OK (float)  max_abs_err={maxd}  mean_abs_err={mean:.2f}")

    # Fixed-point DCT path (what we'll port to C)
    jpeg_i = build_jpeg(rgb, W, H, 50, jhc, use_int=True)
    open("oracle_test_int.jpg", "wb").write(jpeg_i)
    dec_i = np.asarray(Image.open("oracle_test_int.jpg").convert("RGB"))
    diff_i = np.abs(dec_i.astype(int) - arr.astype(int))
    print(f"decode OK (int)    max_abs_err={int(diff_i.max())}  mean_abs_err={float(diff_i.mean()):.2f}")
    assert jpeg_i[:2] == b"\xFF\xD8" and jpeg_i[-2:] == b"\xFF\xD9"
    print("SOI/EOI markers OK")
