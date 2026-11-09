#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""KAO48 (48x48 discoveries / items icons) generative-repaint pipeline helpers.

This script does NOT call any image-generation model (that step is done by the
agent via the ImageGen tool, one icon at a time). It only handles:

  prepare  - build a de-dithered 1024x1024 structure reference from each
             original 48x48 8-color indexed PNG, for image-to-image input.
  finalize - clean the platform watermark off generated images, normalize to
             192x192 RGBA, check silhouette fidelity against the original,
             write manifest.json and a side-by-side contact sheet.

Everything is written to a NEW output directory; the script refuses to run if
the output directory already exists, so existing assets are never overwritten.

Usage:
  python tools/kao48_repaint.py prepare \
      --src assets/original/KAO/discoveries --out hd/kao48-repaint-.../refs

  python tools/kao48_repaint.py finalize \
      --raw hd/kao48-repaint-.../raw \
      --orig assets/original/KAO/discoveries \
      --hq  assets/hd/KAO/discoveries \
      --out hd/kao48-repaint-.../final

Pillow is required (use the Python 3.10 interpreter that has it installed).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

# KAO 8-color VGA palette, identical for discoveries and items.
PALETTE = [
    (0, 0, 0),        # 0 black
    (0, 160, 96),     # 1 teal green
    (208, 64, 0),     # 2 brick red
    (240, 160, 96),   # 3 orange tan / skin
    (0, 64, 208),     # 4 deep blue
    (0, 160, 240),    # 5 sky blue
    (208, 96, 160),   # 6 pink
    (240, 224, 208),  # 7 cream white
]

TARGET_SIZE = 192          # engine expects 4x of 48x48
REF_SIZE = 1024            # image-to-image input resolution
# Platform watermark occupies roughly the bottom-right corner of the 1024 image.
WATERMARK_BOX = (0.865, 0.925)  # (x0_ratio, y0_ratio)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def indexed_to_rgb(path: Path) -> tuple[Image.Image, list[int]]:
    im = Image.open(path).convert("P")
    idx = list(im.getdata())
    rgb = Image.new("RGB", im.size)
    rgb.putdata([PALETTE[i] for i in idx])
    return rgb, idx


def dedither(idx: list[int], w: int, h: int) -> Image.Image:
    """Sliding-window two-color dither detection.

    The original art mixes several dither phases: 2x2 checkerboards (a,b;b,a),
    1-pixel dot grids (a,b;a,a), alternating rows/columns (a,a;b,b). Restricting
    detection to the pure checkerboard leaves stripe and dot dithering behind,
    which the generative model then mistakes for painted ornament. Any 2x2
    block containing EXACTLY TWO colors is therefore treated as dither and
    replaced by the block's average color. Blocks with 3+ colors (real edges,
    stripes and ornaments) are preserved.
    """
    acc: list[list[tuple[int, int, int]]] = [[] for _ in range(w * h)]
    for y in range(h - 1):
        for x in range(w - 1):
            p = y * w + x
            a, b, c, d = idx[p], idx[p + 1], idx[p + w], idx[p + w + 1]
            if len({a, b, c, d}) == 2:
                col = tuple(sum(PALETTE[v][k] for v in (a, b, c, d)) // 4
                            for k in range(3))
                for q in (p, p + 1, p + w, p + w + 1):
                    acc[q].append(col)
    out = []
    for i, v in enumerate(idx):
        if acc[i]:
            n = len(acc[i])
            out.append(tuple(sum(c[k] for c in acc[i]) // n for k in range(3)))
        else:
            out.append(PALETTE[v])
    r = Image.new("RGB", (w, h))
    r.putdata(out)
    return r


def strip_watermark(im: Image.Image) -> tuple[Image.Image, str]:
    """Remove the platform watermark in the bottom-right corner.

    Strategy: if the region is essentially flat background, tile the column
    just left of the region across it (invisible). Otherwise copy the equally
    sized strip directly above the region down over it, and flag the result
    for manual review in the manifest.
    """
    im = im.convert("RGB")
    w, h = im.size
    x0, y0 = int(w * WATERMARK_BOX[0]), int(h * WATERMARK_BOX[1])
    region = im.crop((x0, y0, w, h))
    ref_col = [im.getpixel((x0 - 1, y)) for y in range(y0, h)]
    base = tuple(sum(c[k] for c in ref_col) // len(ref_col) for k in range(3))

    px = list(region.getdata())
    # Watermark glyphs are near-white; the artwork background is deep navy.
    # Judge by bright/warm "content" pixels instead of overall color drift,
    # otherwise a harmless vertical background gradient looks like content.
    warm = sum(1 for c in px if c[0] > 110 and c[0] >= c[2] * 0.8)
    ratio = warm / max(len(px), 1)

    if ratio < 0.06:
        # flat background: replicate the left edge column row by row
        fill = Image.new("RGB", (w - x0, h - y0))
        fpx = []
        for c in ref_col:
            fpx.extend([c] * (w - x0))
        fill.putdata(fpx)
        im.paste(fill, (x0, y0))
        method = "flat-fill"
    else:
        # content in the corner: borrow the strip above it
        patch = im.crop((x0, y0 - (h - y0), w, y0))
        im.paste(patch, (x0, y0))
        method = "above-strip (REVIEW)"
    return im, method


def subject_mask(im: Image.Image, size: int = 48) -> set[int]:
    """Binary 'non-background' mask.

    The original art's background is a BLACK + DEEP BLUE dither, so BOTH must
    count as background. Counting black as subject makes the original bbox
    collapse into the full canvas and breaks IoU / composition alignment.
    """
    small = im.convert("RGB").resize((size, size), Image.Resampling.BOX)
    mask = set()
    for i, (r, g, b) in enumerate(small.getdata()):
        dark = r < 70 and g < 70 and b < 70
        navy = b > 70 and b > r * 1.3 and g < b
        if not (dark or navy):
            mask.add(i)
    return mask


def silhouette_iou(orig: Image.Image, gen: Image.Image) -> float:
    a = subject_mask(orig)
    b = subject_mask(gen)
    if not a and not b:
        return 1.0
    return len(a & b) / max(len(a | b), 1)


def align_composition(gen: Image.Image, orig: Image.Image) -> tuple[Image.Image, str]:
    """Re-fit the repainted subject to the original silhouette bounding box.

    Generative repaints drift in scale and position (measured IoU ~0.49 on the
    flamingo sample). A single global scale+translate of the WHOLE generated
    image brings the subject back without cutting the painted background.
    Full-bleed scene icons are skipped: they have no isolated subject bbox.
    """
    size = 48
    mo, mg = subject_mask(orig, size), subject_mask(gen, size)
    if not mo or not mg:
        return gen, "skipped (empty mask)"
    if len(mo) / (size * size) > 0.60:
        return gen, "skipped (full-bleed scene)"

    def bbox(m):
        xs = [i % size for i in m]
        ys = [i // size for i in m]
        return min(xs), min(ys), max(xs) + 1, max(ys) + 1

    S = TARGET_SIZE // size
    ob = tuple(v * S for v in bbox(mo))
    gb = tuple(v * S for v in bbox(mg))
    ow, oh = ob[2] - ob[0], ob[3] - ob[1]
    gw, gh = gb[2] - gb[0], gb[3] - gb[1]
    scale = min(ow / max(gw, 1), oh / max(gh, 1))
    if not (0.5 < scale < 2.0):
        return gen, "skipped (scale %.2f out of range)" % scale

    W, H = gen.size
    scaled = gen.resize((max(1, int(W * scale)), max(1, int(H * scale))),
                        Image.Resampling.LANCZOS)
    dx = (ob[0] + ob[2]) / 2 - (gb[0] + gb[2]) / 2 * scale
    dy = (ob[1] + ob[3]) / 2 - (gb[1] + gb[3]) / 2 * scale
    corners = [gen.getpixel(p) for p in
               [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)]]
    bgcol = tuple(sorted(c[k] for c in corners)[len(corners) // 2] for k in range(3))
    canvas = Image.new("RGB", (W, H), bgcol)
    canvas.paste(scaled, (int(dx), int(dy)))
    return canvas, "aligned scale=%.3f offset=(%d,%d)" % (scale, int(dx), int(dy))


def cmd_prepare(args) -> int:
    src, out = Path(args.src), Path(args.out)
    if out.exists():
        print("ERROR: output directory already exists, refusing to overwrite: %s" % out)
        return 2
    files = sorted(src.glob("*.png"))
    if not files:
        print("ERROR: no PNG in %s" % src)
        return 2
    out.mkdir(parents=True)
    for f in files:
        rgb, idx = indexed_to_rgb(f)
        ref = dedither(idx, *rgb.size)
        if args.blur > 0:
            # Sub-pixel blur dissolves the residual 1px dot grids that the
            # two-color detector cannot catch; without it the generative model
            # repaints those dots as mosaic tiles / gems / studs.
            ref = ref.filter(ImageFilter.GaussianBlur(args.blur))
        ref.resize((REF_SIZE, REF_SIZE), Image.Resampling.LANCZOS).save(out / f.name)
    print("prepared %d reference images -> %s" % (len(files), out))
    return 0


def find_in(dirs, name):
    for d in dirs:
        p = d / name
        if p.exists():
            return p
    return None


def cmd_finalize(args) -> int:
    raw, out = Path(args.raw), Path(args.out)
    orig_dirs = [Path(d) for d in args.orig]
    hq_dirs = [Path(d) for d in args.hq]
    if out.exists():
        print("ERROR: output directory already exists, refusing to overwrite: %s" % out)
        return 2
    if not raw.exists():
        print("ERROR: raw generated-image directory not found: %s" % raw)
        return 2
    out.mkdir(parents=True)
    (out / "png").mkdir()

    entries = []
    for f in sorted(raw.glob("*.png")):
        stem = f.name
        op = find_in(orig_dirs, stem)
        hp = find_in(hq_dirs, stem)
        clean, method = strip_watermark(Image.open(f))
        render = clean.resize((TARGET_SIZE, TARGET_SIZE), Image.Resampling.LANCZOS)
        align_note = "off"
        if not args.no_align and op is not None:
            render, align_note = align_composition(render, Image.open(op).convert("RGB"))
        final = render
        rgba = final.convert("RGBA")
        rgba.save(out / "png" / stem)

        iou = None
        if op is not None:
            iou = round(silhouette_iou(Image.open(op), final), 4)
        entries.append({
            "composition_align": align_note,
            "id": stem,
            "source": str(op) if op is not None else None,
            "source_sha256": sha256(op) if op is not None else None,
            "generated_raw": str(f),
            "generated_sha256": sha256(f),
            "output": str(out / "png" / stem),
            "size": [TARGET_SIZE, TARGET_SIZE],
            "watermark_removal": method,
            "silhouette_iou": iou,
            "needs_review": (iou is not None and iou < args.iou_threshold)
                            or "REVIEW" in method,
        })

    manifest = {
        "created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_dirs": [str(d) for d in orig_dirs],
        "generated_raw_dir": str(raw),
        "target_size": [TARGET_SIZE, TARGET_SIZE],
        "iou_threshold": args.iou_threshold,
        "count": len(entries),
        "note": "Generative repaint. Original assets are never modified. "
                "A low silhouette_iou means the AI changed the composition and "
                "that icon must be regenerated or manually fixed.",
        "entries": entries,
    }
    (out / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    # contact sheet: original (nearest 4x) | existing hqx HD | generative repaint
    shown = entries[: min(12, len(entries))]
    if shown:
        cell, label = TARGET_SIZE, 22
        sheet = Image.new("RGB", (3 * cell, len(shown) * (cell + label)), (245, 245, 245))
        d = ImageDraw.Draw(sheet)
        for i, e in enumerate(shown):
            y = i * (cell + label)
            o = Image.open(find_in(orig_dirs, e["id"])).convert("RGB").resize(
                (cell, cell), Image.Resampling.NEAREST)
            sheet.paste(o, (0, y))
            hp = find_in(hq_dirs, e["id"])
            if hp is not None:
                sheet.paste(Image.open(hp).convert("RGB"), (cell, y))
            sheet.paste(Image.open(out / "png" / e["id"]).convert("RGB"), (2 * cell, y))
            d.text((4, y + cell + 4), "%s  IoU=%s" % (e["id"], e["silhouette_iou"]),
                   fill=(20, 20, 20))
        sheet.save(out / "contact_sheet.png")

    bad = sum(1 for e in entries if e["needs_review"])
    print("finalized %d icons -> %s" % (len(entries), out))
    print("  needs manual review: %d (IoU < %.2f or corner contained content)"
          % (bad, args.iou_threshold))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("prepare")
    p.add_argument("--src", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--blur", type=float, default=0.0,
                   help="Gaussian blur radius on the 48x48 reference; "
                        "1.0 removes residual dot grids before repaint")
    p.set_defaults(func=cmd_prepare)

    f = sub.add_parser("finalize")
    f.add_argument("--raw", required=True)
    f.add_argument("--orig", required=True, nargs="+",
                   help="one or more original 48x48 directories")
    f.add_argument("--hq", required=True, nargs="+",
                   help="one or more existing HD directories")
    f.add_argument("--out", required=True)
    f.add_argument("--iou-threshold", type=float, default=0.62)
    f.add_argument("--no-align", action="store_true",
                   help="skip subject bounding-box alignment")
    f.set_defaults(func=cmd_finalize)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
