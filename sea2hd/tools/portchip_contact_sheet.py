"""PORTCHIP 材质联系表（方案 B）：每套 atlas 输出 AI 重绘输入图、核对图与索引清单。

每套 atlas 的 240 个 16x16 tile 已按 hqx 4x 放大到磁盘（assets/hd/PORTCHIP/tiles）。
把它们排成 16 列 x 15 行的联系表，交给生成式重绘；重绘后按固定格距切回 240 张。
排序策略：先按材质分类分组，组内按 PORTMAP 统计的 4 邻域共现矩阵的 Fiedler 向量排序，
让真实相邻的材质靠在一起，给模型提供过渡上下文。

输出：docs/materials/atlas-{a}-input.png（无文字，喂给 AI）
     docs/materials/atlas-{a}-ref.png（带 tile 序号与引用次数，人工核对）
     docs/materials/manifest.json（切图公式 + 每个格子的 tile 元数据）
"""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def find_root():
    for p in Path(__file__).resolve().parents:
        if (p / "sea2" / "PORTMAP.LZW").exists():
            return p
    raise SystemExit("找不到游戏根目录（sea2/PORTMAP.LZW）")


ROOT = find_root()
sys.path.insert(0, str(ROOT / "tools"))
from koei_assets import LS11Archive, decode_planar

OUT = ROOT / "docs" / "materials"
OUT.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------- 原始数据
chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, orig = [], []
for e in arc.entries:
    raw = arc.decompress(e)
    if len(raw) == 30720:
        entry_ids.append(e.index)
        orig.append(np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8).reshape(240, 16, 16))
orig = np.array(orig)
NA = len(orig)

marc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = [np.frombuffer(marc.decompress(e), np.uint8).reshape(96, 96)
        for e in marc.entries if e.uncomp_size == 9216]
maps = np.array(maps)
atlas_of = np.array([chip_no[i] if i < len(chip_no) and chip_no[i] < NA else 0
                     for i in range(len(maps))])
print(f"atlas {NA} 套，港口地图 {len(maps)} 张，atlas 分布 {np.bincount(atlas_of, minlength=NA).tolist()}")

hd = np.zeros((NA, 240, 64, 64, 3), np.uint8)
for a, eid in enumerate(entry_ids):
    for t in range(240):
        hd[a, t] = np.array(Image.open(ROOT / "assets" / "hd" / "PORTCHIP" / "tiles" /
                                      f"{eid:03d}_{t:03d}.png").convert("RGB"))

# ---------------------------------------------------------------- 引用次数与邻接共现
usage = np.zeros((NA, 240), np.int32)
cooc = np.zeros((NA, 240, 240), np.float64)
for m, a in zip(maps, atlas_of):
    usage[a] += np.bincount(m.ravel(), minlength=256)[:240]
    for dy, dx in ((0, 1), (1, 0)):
        u = m[:96 - dy, :96 - dx].astype(int).ravel()
        v = m[dy:, dx:].astype(int).ravel()
        cooc[a] += np.bincount(u * 240 + v, minlength=57600).reshape(240, 240)
cooc += cooc.transpose(0, 2, 1)


# ---------------------------------------------------------------- 分类
def classify(t):
    n = len(np.unique(t))
    c = t[1:-1, 1:-1]
    flat = float(((c == t[1:-1, 0:-2]) & (c == t[1:-1, 2:]) &
                  (c == t[0:-2, 1:-1]) & (c == t[2:, 1:-1])).mean())
    if n <= 3 and flat < 0.25:
        return 0
    if flat >= 0.5:
        return 1
    if n <= 8 and flat < 0.2:
        return 2
    return 3


CLS = np.array([[classify(orig[a][t]) for t in range(240)] for a in range(NA)], np.uint8)
CLASS_NAME = ["抖动水面", "平涂地块", "细纹理", "建筑结构"]

FAM = [[3, 8, 9, 10, 11], [5, 6, 13, 14, 15], [2, 7]]
SHARE = np.zeros((NA, 240, 3), np.float32)
for a in range(NA):
    px = orig[a].reshape(240, 256)
    for f, fam in enumerate(FAM):
        SHARE[a, :, f] = np.isin(px, fam).mean(1)


def fiedler(c):
    d = c.sum(1)
    if d.sum() == 0:
        return np.zeros(240)
    _, vec = np.linalg.eigh(np.diag(d) - c)
    return vec[:, 1]


COLS, ROW_N, PITCH, TILE, MARGIN = 16, 15, 72, 64, 20
F = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 13)
FT = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 18)

manifest = {
    "pitch": PITCH, "tile": TILE, "cols": COLS, "rows": ROW_N, "margin": MARGIN,
    "slice": "cell i -> x = 20 + (i%16)*72 + 4, y = 20 + (i//16)*72 + 4, size 64",
    "atlases": [],
}

for a in range(NA):
    order = np.lexsort((fiedler(cooc[a]), CLS[a]))
    sheet = np.full((ROW_N * PITCH + 2 * MARGIN, COLS * PITCH + 2 * MARGIN, 3), 24, np.uint8)
    for i, t in enumerate(order):
        r, c = divmod(int(i), COLS)
        y, x = MARGIN + r * PITCH + 4, MARGIN + c * PITCH + 4
        sheet[y:y + TILE, x:x + TILE] = hd[a, t]
    Image.fromarray(sheet).save(OUT / f"atlas-{a}-input.png", optimize=True)

    rh = TILE + 18
    top = MARGIN + 54
    ref = Image.new("RGB", (COLS * PITCH + 2 * MARGIN, top + ROW_N * rh + MARGIN), (16, 18, 22))
    d = ImageDraw.Draw(ref)
    used = int((usage[a] > 0).sum())
    d.text((MARGIN, MARGIN), f"atlas {a} · entry {entry_ids[a]:03d} · 港口 {int((atlas_of == a).sum())}"
                             f" · 被引用 {used}/240", font=FT, fill=(238, 240, 246))
    d.text((MARGIN, MARGIN + 26), " · ".join(
        f"{CLASS_NAME[k]} {int((CLS[a] == k).sum())}" for k in range(4)), font=F, fill=(150, 158, 172))
    for i, t in enumerate(order):
        r, c = divmod(int(i), COLS)
        x, y = MARGIN + c * PITCH + 4, top + r * rh
        ref.paste(Image.fromarray(hd[a, t]), (x, y))
        d.text((x, y + TILE + 1), f"{t:03d} u{int(usage[a, t])}", font=F,
               fill=(210, 216, 228) if usage[a, t] else (96, 102, 114))
    ref.save(OUT / f"atlas-{a}-ref.png", optimize=True)

    cells = []
    for i, t in enumerate(order):
        cells.append({"cell": int(i), "tile": int(t), "class": CLASS_NAME[CLS[a, t]],
                      "used": int(usage[a, t]),
                      "water": round(float(SHARE[a, t, 0]), 3),
                      "sand": round(float(SHARE[a, t, 1]), 3),
                      "green": round(float(SHARE[a, t, 2]), 3),
                      "hash": hashlib.sha1(orig[a][t].tobytes()).hexdigest()[:10]})
    manifest["atlases"].append({"atlas": a, "entry": int(entry_ids[a]),
                                "maps": int((atlas_of == a).sum()), "used_tiles": used,
                                "cells": cells})
    print(f"atlas {a} entry {entry_ids[a]:03d}: 港口 {int((atlas_of == a).sum())}，"
          f"引用 {used}/240，分类 {np.bincount(CLS[a], minlength=4).tolist()}")

marks = {}
for a in range(NA):
    for t in range(240):
        marks.setdefault(hashlib.sha1(orig[a][t].tobytes()).hexdigest(), []).append((a, t))
dup = sum(len(v) for v in marks.values() if len(v) > 1)

used_only = {(a, t) for a in range(NA) for t in range(240) if usage[a, t] > 0}
unique_used = {hashlib.sha1(orig[a][t].tobytes()).hexdigest() for a, t in used_only}

manifest["unique_tiles"] = len(marks)
manifest["duplicate_instances"] = dup
manifest["used_instances"] = len(used_only)
manifest["unique_used_tiles"] = len(unique_used)
(OUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")
print(f"全局唯一 tile {len(marks)}/{NA * 240}，其中被引用 {len(used_only)} 个，去重后 {len(unique_used)} 个"
      f"（重复实例 {dup}）")
print(f"输出目录：{OUT}")
