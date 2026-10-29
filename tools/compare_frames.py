#!/usr/bin/env python3
"""Compare two captured frames (docs/PLAN.md section 3.3).

    python tools/compare_frames.py reference.png candidate.png [--diff out.png]
        [--threshold 24] [--crop x,y,w,h] [--resize]

Reports the share of pixels that differ by more than the threshold (the
largest channel difference, 0..255), the mean absolute difference, the
dominant colours of each image, and the largest differing rectangles.  The
exit status is 0 when the differing share is at most --max-diff (default
1%), so a corpus script can gate on it.

Frames from the PSP, PPSSPP and Dolphin rarely have the same size or
border: --crop takes the picture out of a window capture and --resize scales
the candidate to the reference's size first.  Game-derived images are never
committed; keep the corpus outside the repository.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image


def load(path: str, crop: tuple[int, int, int, int] | None) -> np.ndarray:
    img = Image.open(path).convert("RGB")
    if crop:
        x, y, w, h = crop
        img = img.crop((x, y, x + w, y + h))
    return np.asarray(img, dtype=np.int16)


def dominant_colours(a: np.ndarray, n: int = 5) -> list[tuple[str, float]]:
    """The most common colours, quantised to 4 bits per channel."""
    q = (a >> 4).reshape(-1, 3)
    keys = q[:, 0] * 256 + q[:, 1] * 16 + q[:, 2]
    values, counts = np.unique(keys, return_counts=True)
    order = np.argsort(counts)[::-1][:n]
    out = []
    for v, c in zip(values[order], counts[order]):
        r, g, b = (v >> 8) & 15, (v >> 4) & 15, v & 15
        out.append((f"#{r * 17:02X}{g * 17:02X}{b * 17:02X}", c / len(keys)))
    return out


def regions(mask: np.ndarray, limit: int = 5) -> list[tuple[int, int, int, int, int]]:
    """Bounding boxes (x, y, w, h, pixels) of the largest connected differing
    areas, on an 8x8 grid so a noisy edge does not split into thousands."""
    h, w = mask.shape
    gh, gw = (h + 7) // 8, (w + 7) // 8
    pad = np.zeros((gh * 8, gw * 8), dtype=bool)
    pad[:h, :w] = mask
    grid = pad.reshape(gh, 8, gw, 8).sum(axis=(1, 3))
    seen = np.zeros_like(grid, dtype=bool)
    boxes = []
    for gy in range(gh):
        for gx in range(gw):
            if grid[gy, gx] == 0 or seen[gy, gx]:
                continue
            stack = [(gy, gx)]
            seen[gy, gx] = True
            x0 = x1 = gx
            y0 = y1 = gy
            total = 0
            while stack:
                cy, cx = stack.pop()
                total += int(grid[cy, cx])
                x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < gh and 0 <= nx < gw and grid[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            boxes.append((x0 * 8, y0 * 8, (x1 - x0 + 1) * 8, (y1 - y0 + 1) * 8, total))
    boxes.sort(key=lambda b: b[4], reverse=True)
    return boxes[:limit]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--diff", help="write a difference image (differing pixels in red)")
    ap.add_argument("--threshold", type=int, default=24, help="per-pixel tolerance (default 24)")
    ap.add_argument("--max-diff", type=float, default=1.0, help="pass if at most this %% of pixels differ")
    ap.add_argument("--crop", help="x,y,w,h applied to both images before comparing")
    ap.add_argument("--resize", action="store_true", help="scale the candidate to the reference size")
    args = ap.parse_args()

    crop = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
    ref = load(args.reference, crop)
    cand = load(args.candidate, crop)
    if cand.shape != ref.shape:
        if not args.resize:
            print(f"size mismatch: {ref.shape[1]}x{ref.shape[0]} vs {cand.shape[1]}x{cand.shape[0]} "
                  "(use --resize or --crop)")
            return 2
        img = Image.fromarray(cand.astype(np.uint8)).resize((ref.shape[1], ref.shape[0]), Image.BILINEAR)
        cand = np.asarray(img, dtype=np.int16)

    delta = np.abs(ref - cand).max(axis=2)
    mask = delta > args.threshold
    share = 100.0 * mask.mean()
    print(f"size        {ref.shape[1]}x{ref.shape[0]}")
    print(f"differing   {share:.3f}% of pixels (threshold {args.threshold})")
    print(f"mean |d|    {np.abs(ref - cand).mean():.2f}")
    for name, img in (("reference", ref), ("candidate", cand)):
        cols = ", ".join(f"{c} {p * 100:.1f}%" for c, p in dominant_colours(img))
        print(f"{name:<11} {cols}")
    for x, y, w, h, n in regions(mask):
        print(f"region      x={x} y={y} w={w} h={h} ({n} px)")
    if args.diff:
        out = (cand // 3).astype(np.uint8)
        out[mask] = (255, 0, 0)
        Image.fromarray(out).save(args.diff)
    ok = share <= args.max_diff
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
