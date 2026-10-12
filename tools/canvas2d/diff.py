#!/usr/bin/env python3
# Pixel diff of two same-size PNGs: mean absolute error (0-255, RGB), PSNR, and the share of
# pixels whose largest channel difference exceeds 8 / 32 / 64. Writes an amplified diff image.
#   python3 -I tools/canvas2d/diff.py a.png b.png [diff.png]
import sys
import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.int16)
if a.shape != b.shape:
    print(f'size mismatch {a.shape} vs {b.shape}')
    sys.exit(2)
d = np.abs(a - b)
mx = d.max(axis=2)
mse = (d.astype(np.float64) ** 2).mean()
psnr = float('inf') if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
n = mx.size
print(f'MAE {d.mean():.3f}  PSNR {psnr:.2f} dB  >8: {100 * (mx > 8).sum() / n:.2f}%  '
      f'>32: {100 * (mx > 32).sum() / n:.2f}%  >64: {100 * (mx > 64).sum() / n:.2f}%  max {mx.max()}')
if len(sys.argv) > 3:
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(sys.argv[3])
