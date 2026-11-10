"""材质聚类验证（方案 B）：把 1680 个 tile 全局聚成材质类，并用「聚类代表图」重拼 101 张地图，
测量还原误差，判断「按材质替换」是否安全。

特征（每个 tile 一组，分块归一化后加权）：
  hist16    调色板颜色直方图（16 维）
  border16  边缘 1px 的颜色直方图（16 维），区分「物件」与「材质」
  edge4x4   4x4 块的局部边缘能量（16 维），区分平涂与有结构的格子

输出：docs/materials/classes.json
     docs/materials/class-sheet.png
     docs/materials/class-recon.png
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(r"d:\OldGames\大航海时代2")
sys.path.insert(0, str(ROOT / "tools"))
from koei_assets import LS11Archive, decode_planar, PORTCHIP_PALETTES

OUT = ROOT / "docs" / "materials"
OUT.mkdir(parents=True, exist_ok=True)
PAL = np.array(PORTCHIP_PALETTES["day"], np.uint8)

K = 16
FAMILY = {"水": [3, 8, 9, 10, 11], "沙": [5, 6, 13, 14, 15], "草": [2, 7],
          "暗": [0, 1], "红": [4, 12]}

# ---------------------------------------------------------------- 数据
chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, tiles = [], []            # tiles[a][t] = 16x16 index image
for e in arc.entries:
    raw = arc.decompress(e)
    if len(raw) == 30720:
        entry_ids.append(e.index)
        tiles.append(np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8).reshape(240, 16, 16))
tiles = np.array(tiles)
NA = len(tiles)

marc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = np.array([np.frombuffer(marc.decompress(e), np.uint8).reshape(96, 96)
                 for e in marc.entries if e.uncomp_size == 9216])
atlas_of = np.array([chip_no[i] if i < len(chip_no) and chip_no[i] < NA else 0 for i in range(len(maps))])

# ---------------------------------------------------------------- 唯一 tile
flat = tiles.reshape(-1, 256)
ukey, inv = np.unique(flat, axis=0, return_inverse=True)
N = len(ukey)
source = [[(a, t) for a in range(NA) for t in range(240) if inv[a * 240 + t] == u] for u in range(N)]
print(f"唯一 tile {N}（{NA} 套共 {NA * 240} 格）")


def feats(px):
    h = np.bincount(px.ravel(), minlength=16).astype(np.float32)
    h /= h.sum()
    border = np.concatenate([px[0], px[-1], px[:, 0], px[:, -1]])
    b = np.bincount(border, minlength=16).astype(np.float32)
    b /= b.sum()
    g = px.astype(np.float32)
    gy = np.abs(np.diff(g, axis=0))
    gx = np.abs(np.diff(g, axis=1))
    e = np.zeros((4, 4), np.float32)
    for by in range(4):
        for bx in range(4):
            e[by, bx] = gy[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].mean() + \
                        gx[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].mean()
    e = e.ravel() / (e.max() + 1e-6)
    return np.concatenate([h * 2.0, b * 1.6, e * 1.2])


F = np.array([feats(ukey[u].reshape(16, 16)) for u in range(N)], np.float32)
W = np.array([2.0] * 16 + [1.6] * 16 + [1.2] * 16, np.float32)
W /= np.linalg.norm(W)


def kmeans(f, k, iters=40, seed=0):
    rng = np.random.default_rng(seed)
    c = f[rng.choice(len(f), k, replace=False)].copy()
    for _ in range(iters):
        d = ((f[:, None, :] - c[None]) ** 2).sum(2)
        lab = d.argmin(1)
        new = c.copy()
        for j in range(k):
            if (lab == j).any():
                new[j] = f[lab == j].mean(0)
        if np.allclose(new, c):
            break
        c = new
    d = ((f[:, None, :] - c[None]) ** 2).sum(2)
    return d.argmin(1), c, d.min(1).mean()


for k in (8, 12, K, 24, 32):
    _, _, disp = kmeans(F, k)
    print(f"k={k}: 平均簇内距离 {disp:.3f}")

lab, cents, _ = kmeans(F, K)
lab_count = np.bincount(lab, minlength=K)

# ---------------------------------------------------------------- 代表图（medoid）与统计
medoid = np.zeros(K, int)
for j in range(K):
    mem = np.where(lab == j)[0]
    if len(mem) == 1:
        medoid[j] = mem[0]
        continue
    d = ((F[mem][:, None, :] - F[mem][None]) ** 2).sum(2)
    medoid[j] = mem[d.sum(1).argmin()]

cov = np.zeros(K, np.int64)
for m, a in zip(maps, atlas_of):
    for t in range(240):
        cov[lab[inv[a * 240 + t]]] += int((m == t).sum())
total_cells = 101 * 96 * 96
share = cov / total_cells

# ---------------------------------------------------------------- 用代表图重拼，测误差
# repof[a][t] = 该 tile 的聚类代表图（16x16 索引）
repof = np.zeros((NA, 240, 16, 16), np.uint8)
for a in range(NA):
    for t in range(240):
        repof[a, t] = ukey[medoid[lab[inv[a * 240 + t]]]].reshape(16, 16)

diffs = []
for pi in range(len(maps)):
    a = atlas_of[pi]
    ix = maps[pi]
    diffs.append(float((tiles[a][ix] != repof[a][ix]).mean()))
print(f"重拼误差：平均 {np.mean(diffs) * 100:.1f}%，最好 {min(diffs) * 100:.1f}%，最差 {max(diffs) * 100:.1f}%")

# 每个 tile 被替换成代表图后，有多少实例「本就与代表图完全相同」
same = int(sum(inv[a * 240 + t] == medoid[lab[inv[a * 240 + t]]]
               for a in range(NA) for t in range(240)))
print(f"实例级：{same}/{NA * 240} 格所选代表图与其原 tile 像素完全相同")

# ---------------------------------------------------------------- 命名
def label_of(j):
    mem = np.where(lab == j)[0]
    px = ukey[mem].reshape(-1, 256)
    h = np.bincount(px.ravel(), minlength=16).astype(float)
    h /= h.sum()
    fam = {k: h[v].sum() for k, v in FAMILY.items()}
    top = sorted(fam.items(), key=lambda kv: -kv[1])[:2]
    return "+".join(f"{k}{v * 100:.0f}%" for k, v in top)


order = np.argsort(-share)

# ---------------------------------------------------------------- 类目表
FS = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 15)
FB = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 22)
CW, CH, GAP = 64, 64, 8
LABW = 300
sheetW = LABW + 10 * (CW + GAP) + GAP * 2
rowH = CH + 16
sheet = Image.new("RGB", (sheetW, 54 + len(order) * rowH + 16), (16, 18, 22))
d = ImageDraw.Draw(sheet)
d.text((12, 12), f"材质聚类验证：{N} 个唯一 tile → {K} 类（按地图覆盖率排序）", font=FB, fill=(240, 242, 248))
d.text((12, 34), "左侧为该类在地图上的覆盖占比与主色构成；右侧最多显示 10 个成员 tile（4x hqx）",
       font=FS, fill=(150, 158, 172))
hd_tile = {}
for a, eid in enumerate(entry_ids):
    for t in range(240):
        hd_tile[(a, t)] = Image.open(ROOT / "assets" / "hd" / "PORTCHIP" / "tiles" /
                                    f"{eid:03d}_{t:03d}.png").convert("RGB")

cnt = np.bincount(inv, minlength=N)
for i, j in enumerate(order):
    y = 54 + i * rowH
    d.text((12, y + 4), f"#{i:02d}  {share[j] * 100:5.1f}%  {label_of(j)}", font=FS, fill=(226, 230, 240))
    d.text((12, y + 26), f"{lab_count[j]} 个成员  medoid {source[medoid[j]][0][0]}/{source[medoid[j]][0][1]:03d}",
           font=FS, fill=(140, 148, 162))
    show = sorted(np.where(lab == j)[0], key=lambda u: -int(cnt[u]))[:10]
    for n, u in enumerate(show):
        a, t = source[u][0]
        sheet.paste(hd_tile[(a, t)], (LABW + n * (CW + GAP), y))
sheet.save(OUT / "class-sheet.png", optimize=True)
print("wrote class-sheet.png", sheet.size)

# ---------------------------------------------------------------- 还原对照（port 42）
PI = 42
a = atlas_of[PI]


def render(use_rep):
    if use_rep:
        px = repof[a][maps[PI]]
    else:
        px = tiles[a][maps[PI]]
    out = np.zeros((96 * 16, 96 * 16, 3), np.uint8)
    for ty in range(96):
        for tx in range(96):
            out[ty * 16:(ty + 1) * 16, tx * 16:(tx + 1) * 16] = PAL[px[ty, tx]]
    return out


o1, o2 = render(False), render(True)
diffmask = (o1 != o2).any(2)
zoom = 512
panels = [("原版（真实 tile）", o1), ("聚类代表图重拼", o2)]
W2 = 3 * zoom + 4 * 24
H2 = 60 + zoom + 90
img = Image.new("RGB", (W2, H2), (16, 18, 22))
dd = ImageDraw.Draw(img)
dd.text((24, 16), f"用「聚类代表图」重拼 port {PI}：像素差异 {diffmask.mean() * 100:.1f}%",
        font=FB, fill=(240, 242, 248))
for i, (name, im) in enumerate(panels):
    img.paste(Image.fromarray(im).resize((zoom, zoom), Image.Resampling.LANCZOS), (24 + i * (zoom + 24), 60))
    dd.text((24 + i * (zoom + 24), 60 + zoom + 8), name, font=FS, fill=(210, 216, 228))
dm = (np.repeat(np.repeat(diffmask, 1, 0), 1, 1).astype(np.uint8) * 255)
img.paste(Image.fromarray(dm).resize((zoom, zoom), Image.Resampling.NEAREST).convert("RGB"),
          (24 + 2 * (zoom + 24), 60))
dd.text((24 + 2 * (zoom + 24), 60 + zoom + 8), "差异位置（白=被改动）", font=FS, fill=(210, 216, 228))
dd.text((24, 60 + zoom + 36), f"该图实际差异像素 {diffmask.mean() * 100:.1f}%；若按类替换材质，"
        f"整张地图只有这一部分像素会变化，其余完全一致。", font=FS, fill=(150, 158, 172))
img.save(OUT / "class-recon.png", optimize=True)
print("wrote class-recon.png", img.size)

# ---------------------------------------------------------------- 清单
data = {"k": K, "unique_tiles": N, "mean_recon_diff": float(np.mean(diffs)),
        "classes": []}
for i, j in enumerate(order):
    mem = np.where(lab == j)[0]
    data["classes"].append({
        "rank": i, "cluster": int(j), "share": round(float(share[j]), 4),
        "members": len(mem), "label": label_of(j),
        "medoid": {"atlas": int(source[medoid[j]][0][0]), "tile": int(source[medoid[j]][0][1])},
        "tiles": [[int(a), int(t)] for u in mem for a, t in source[u]],
    })
(OUT / "classes.json").write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")

print("\n按覆盖排序的类目：")
for i, j in enumerate(order):
    print(f"  #{i:02d} {share[j] * 100:5.1f}%  {lab_count[j]:3d} 员  {label_of(j)}")
