#!/usr/bin/env python3
"""
split_at_decimal.py
-------------------------------------------------------------------------------
Take a PNG of a floating-point number (e.g. "17.71", "1,234.56", "2.28") and
split it into two images at the decimal separator -- the integer part on the
left and the fractional part on the right.

How the decimal is found (no OCR needed):
  The separator is the small ink blob that sits low on the baseline, between the
  tall digit glyphs. The script binarizes the image (auto-detecting text
  polarity), finds connected components, measures the digit band, and selects
  the small, low, roughly-square component nearest the baseline. When more than
  one qualifies (e.g. a thousands comma AND a decimal point), the right-most one
  wins -- the decimal separator always comes after any grouping separators.

Usage:
  python split_at_decimal.py NUMBER.png
  python split_at_decimal.py NUMBER.png -o out_dir --trim --debug
  python split_at_decimal.py NUMBER.png --keep-dot left

Outputs (next to the input by default):
  <stem>_left.png    integer part
  <stem>_right.png   fractional part
  <stem>_debug.png   (only with --debug) overlay showing the detection

Exit code 0 on success, 2 if no decimal separator could be located.
-------------------------------------------------------------------------------
"""

import argparse
import os
import sys

import cv2
import numpy as np


def load_image(path):
    """Load a PNG keeping its channels; return (original_array, grayscale).

    A 4-channel image is composited onto white before graying so transparent
    pixels read as background, not black.
    """
    orig = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if orig is None:
        sys.exit(f"ERROR: could not read image: {path}")

    if orig.ndim == 2:                       # already grayscale
        gray = orig
    elif orig.shape[2] == 4:                 # BGRA -> composite on white
        bgr = orig[:, :, :3].astype(np.float32)
        alpha = (orig[:, :, 3].astype(np.float32) / 255.0)[..., None]
        comp = (bgr * alpha + 255.0 * (1.0 - alpha)).astype(np.uint8)
        gray = cv2.cvtColor(comp, cv2.COLOR_BGR2GRAY)
    else:                                    # BGR
        gray = cv2.cvtColor(orig, cv2.COLOR_BGR2GRAY)
    return orig, gray


def binarize(gray):
    """Otsu threshold with ink forced to white (foreground = minority pixels)."""
    _, th = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    if cv2.countNonZero(th) > th.size / 2:   # ink should be the minority
        th = cv2.bitwise_not(th)
    return th


def find_decimal(th, min_area):
    """Locate the decimal separator component.

    Returns (left, top, width, height) of the chosen component, or None.
    """
    num, _, stats, _ = cv2.connectedComponentsWithStats(th, connectivity=8)

    # Collect real components (skip background label 0 and specks).
    comps = []
    for i in range(1, num):
        x, y, w, h, area = stats[i]
        if area >= min_area and w >= 1 and h >= 1:
            comps.append((x, y, w, h, area))
    if not comps:
        return None

    # The digit band: tall components define cap height and baseline.
    max_h = max(c[3] for c in comps)
    digits = [c for c in comps if c[3] >= 0.6 * max_h]
    if not digits:
        digits = comps
    baseline = max(y + h for (x, y, w, h, a) in digits)
    cap_top = min(y for (x, y, w, h, a) in digits)
    band = max(1, baseline - cap_top)

    # A decimal separator is small, low, near the baseline and roughly square
    # (this rejects a minus sign / dash, which is wide and vertically centered).
    candidates = []
    for (x, y, w, h, a) in comps:
        cy = y + h / 2.0
        short = h <= 0.55 * band
        narrow = w <= 0.70 * band
        low = cy >= cap_top + 0.45 * band
        on_baseline = (y + h) >= baseline - 0.30 * band
        squarish = 0.4 <= (w / float(h)) <= 2.5
        if short and narrow and low and on_baseline and squarish:
            candidates.append((x, y, w, h, a))

    if not candidates:
        return None

    # Right-most candidate = the decimal (any grouping separators are to its left).
    candidates.sort(key=lambda c: c[0] + c[2])   # by right edge
    return candidates[-1][:4]


def trim(img, pad):
    """Tight-crop an image to its ink, leaving `pad` px of margin."""
    if img.size == 0:
        return img
    gray = img if img.ndim == 2 else cv2.cvtColor(
        img[:, :, :3] if img.ndim == 3 and img.shape[2] == 4 else img,
        cv2.COLOR_BGR2GRAY)
    th = binarize(gray)
    ys, xs = np.where(th > 0)
    if len(xs) == 0:
        return img                            # nothing to trim
    x0, x1 = xs.min(), xs.max() + 1
    y0, y1 = ys.min(), ys.max() + 1
    x0 = max(0, x0 - pad); y0 = max(0, y0 - pad)
    x1 = min(img.shape[1], x1 + pad); y1 = min(img.shape[0], y1 + pad)
    return img[y0:y1, x0:x1]


def main():
    ap = argparse.ArgumentParser(
        description="Split a PNG of a float into integer/fractional halves at the decimal.")
    ap.add_argument("input", help="path to the PNG of the number")
    ap.add_argument("-o", "--outdir", default=None,
                    help="output directory (default: same folder as the input)")
    ap.add_argument("--prefix", default=None,
                    help="output base name (default: input file stem)")
    ap.add_argument("--keep-dot", choices=["drop", "left", "right"], default="drop",
                    help="what to do with the decimal pixels (default: drop)")
    ap.add_argument("--trim", action="store_true",
                    help="tight-crop each half to its ink")
    ap.add_argument("--pad", type=int, default=2,
                    help="padding px kept around ink when --trim (default: 2)")
    ap.add_argument("--min-area", type=int, default=2,
                    help="ignore connected components smaller than this area (default: 2)")
    ap.add_argument("--debug", action="store_true",
                    help="also write <stem>_debug.png showing the detection")
    args = ap.parse_args()

    orig, gray = load_image(args.input)
    h_img, w_img = gray.shape[:2]
    th = binarize(gray)

    dot = find_decimal(th, args.min_area)
    if dot is None:
        print("No decimal separator found.", file=sys.stderr)
        print("  Tips: make sure the image is a single number; try --min-area 1.",
              file=sys.stderr)
        sys.exit(2)

    dx, dy, dw, dh = dot
    dleft, dright = dx, dx + dw
    print(f"Decimal separator at x=[{dleft}..{dright}] "
          f"(w={dw}, h={dh}); image is {w_img}x{h_img}.")

    # Choose the cut. "drop" removes the separator pixels entirely.
    if args.keep_dot == "drop":
        left_img, right_img = orig[:, :dleft], orig[:, dright:]
    elif args.keep_dot == "left":
        left_img, right_img = orig[:, :dright], orig[:, dright:]
    else:  # right
        left_img, right_img = orig[:, :dleft], orig[:, dleft:]

    if left_img.shape[1] == 0 or right_img.shape[1] == 0:
        print("WARNING: one half is empty -- the separator may be at an edge.",
              file=sys.stderr)

    if args.trim:
        left_img = trim(left_img, args.pad)
        right_img = trim(right_img, args.pad)

    outdir = args.outdir or os.path.dirname(os.path.abspath(args.input))
    os.makedirs(outdir, exist_ok=True)
    stem = args.prefix or os.path.splitext(os.path.basename(args.input))[0]
    left_path = os.path.join(outdir, f"{stem}_left.png")
    right_path = os.path.join(outdir, f"{stem}_right.png")

    cv2.imwrite(left_path, left_img)
    cv2.imwrite(right_path, right_img)
    print(f"Wrote: {left_path}  ({left_img.shape[1]}x{left_img.shape[0]})")
    print(f"Wrote: {right_path}  ({right_img.shape[1]}x{right_img.shape[0]})")

    if args.debug:
        dbg = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
        cv2.rectangle(dbg, (dleft, dy), (dright, dy + dh), (0, 0, 255), 1)   # decimal
        cut = dleft if args.keep_dot != "right" else dleft
        cv2.line(dbg, (cut, 0), (cut, h_img - 1), (0, 200, 0), 1)            # cut line
        dbg_path = os.path.join(outdir, f"{stem}_debug.png")
        cv2.imwrite(dbg_path, dbg)
        print(f"Wrote: {dbg_path}")


if __name__ == "__main__":
    main()
