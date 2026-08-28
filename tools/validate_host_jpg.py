"""Validate the host-built JPEG (produced by the FIRMWARE's soft_mjpeg.c)
against the expected 256x240 test pattern. Proves the encoder is correct,
not just decodable."""
import sys
from PIL import Image
import numpy as np

W, H = 256, 240

def expand(p):
    r = (p >> 11) & 0x1F; r = (r << 3) | (r >> 2)
    g = (p >> 5) & 0x3F;  g = (g << 2) | (g >> 4)
    b = p & 0x1F;         b = (b << 3) | (b >> 2)
    return (r, g, b)

bars = [0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF, 0x0000]
exp = np.zeros((H, W, 3), dtype=np.int32)
for y in range(H):
    for x in range(W):
        if y < H - 48:
            px = bars[x // (W // 8)]
        else:
            g = (x * 255) // (W - 1)
            r = (g >> 3) & 0x1F; gg = (g >> 2) & 0x3F; b = (g >> 3) & 0x1F
            px = (r << 11) | (gg << 5) | b
        exp[y, x, 0], exp[y, x, 1], exp[y, x, 2] = expand(px)

im = Image.open("host_test.jpg").convert("RGB")
print("decoded size:", im.size)
assert im.size == (W, H), "size mismatch!"

dec = np.asarray(im, dtype=np.int32)
diff = np.abs(dec.astype(np.int32) - exp.astype(np.int32))
max_err = int(diff.max())
mean_err = float(diff.mean())
print(f"max_abs_err = {max_err}")
print(f"mean_abs_err = {mean_err:.3f}")
print(f"per-channel max: R={int(diff[:,:,0].max())} G={int(diff[:,:,1].max())} B={int(diff[:,:,2].max())}")

# Structural checks: bar centers should match expected bar colour
ok = True
for i in range(8):
    cx = i * (W // 8) + (W // 16)
    got = tuple(int(v) for v in dec[10, cx])
    expc = tuple(int(v) for v in exp[10, cx])
    err = max(abs(got[j] - expc[j]) for j in range(3))
    if err > 16:
        ok = False
    print(f"bar {i}: got={got} exp={expc} err={err}")

# Gradient strip monotonic brightness left->right
gstrip = dec[H - 24, :].mean(axis=1)
mono = bool(np.all(np.diff(gstrip) >= -2))
print(f"gradient monotonic increasing: {mono}")

# Save decoded + expected for visual proof
im.save("host_test_decoded.png")
Image.fromarray(exp.astype(np.uint8)).save("expected_pattern.png")
print("saved host_test_decoded.png, expected_pattern.png")

verdict = (max_err <= 24) and ok and mono
print("VERDICT:", "PASS" if verdict else "FAIL")
sys.exit(0 if verdict else 1)
