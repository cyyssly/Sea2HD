"""Port-map assembly + tile-upscaling comparison.

Root-cause fix: each port map selects its PORTCHIP atlas with CHIP_NO.DAT
(`chip_no[port]` -> atlas entry `chip_no*2`), exactly like the game does.
The previous version of this tool always used atlas 000, which garbles every
port that uses another atlas.

Inputs (read-only): sea2/PORTMAP.LZW, sea2/PORTCHIP.LZW, sea2/CHIP_NO.DAT,
                    assets/hd/PORTCHIP/tiles
Output: docs/port-tile-hd-compare.png
"""
import sys, subprocess, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(r"d:\OldGames\大航海时代2")
sys.path.insert(0, str(ROOT / "tools"))
from koei_assets import LS11Archive, decode_planar, PORTCHIP_PALETTES

S = 4
TS = 16 * S                      # 64 px per tile at 4x
PAL = np.array(PORTCHIP_PALETTES["day"], dtype=np.uint8)

# ---------------------------------------------------------------- data
chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())

arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
orig = []                        # orig[a] = 240x16x16 index image
entry_ids = []
for e in arc.entries:
    raw = arc.decompress(e)
    if len(raw) == 30720:
        px = np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8).reshape(240, 16, 16)
        orig.append(px)
        entry_ids.append(e.index)
print("atlases:", len(orig), "entries:", entry_ids)

hd = []
for a, eid in enumerate(entry_ids):
    hd.append(np.array([np.array(Image.open(ROOT / "assets" / "hd" / "PORTCHIP" / "tiles" /
                                        f"{eid:03d}_{t:03d}.png").convert("RGB"), np.uint8)
                        for t in range(240)]))
print("hd sets:", len(hd), hd[0].shape)

marc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = []
for e in marc.entries:
    raw = marc.decompress(e)
    if len(raw) == 9216:
        maps.append(np.frombuffer(raw, np.uint8).reshape(96, 96))
maps = np.array(maps)
print("maps:", maps.shape)


# ---------------------------------------------------------------- classify per atlas
def classify(t):
    n = len(np.unique(t))
    c = t[1:-1, 1:-1]
    same = ((c == t[1:-1, 0:-2]) & (c == t[1:-1, 2:]) & (c == t[0:-2, 1:-1]) & (c == t[2:, 1:-1]))
    flat = float(same.mean())
    if n <= 3 and flat < 0.25:
        return 0     # dither
    if flat >= 0.5:
        return 1     # flat
    if n <= 8 and flat < 0.2:
        return 2     # texture
    return 3         # structure


CLS = np.array([[classify(orig[a][t]) for t in range(240)] for a in range(len(orig))], np.uint8)
print("class mix (atlas 0):", np.bincount(CLS[0], minlength=4))

# ---------------------------------------------------------------- window pick
def atlas_of(pi):
    return chip_no[pi] if pi < len(chip_no) else 0

best = None
for pi in range(len(maps)):
    if atlas_of(pi) == 0:            # the root-cause panel needs a port whose atlas != 0
        continue
    g = CLS[atlas_of(pi)][maps[pi]]
    for oy in range(0, 96 - 30 + 1, 3):
        for ox in range(0, 96 - 40 + 1, 3):
            w = g[oy:oy + 30, ox:ox + 40]
            cnt = np.bincount(w.ravel(), minlength=4)
            score = cnt[3] * 12 + min(cnt[0] // 2, 90) + min(cnt[1] // 3, 70) + min(cnt[2] // 2, 70)
            if best is None or score > best[0]:
                best = (score, pi, ox, oy)
score, PI, OX, OY = best
A = atlas_of(PI)
print(f"map {PI} window ({OX},{OY}) atlas {A} (chip_no={A}) score {score} mix",
      np.bincount(CLS[A][maps[PI][OY:OY + 30, OX:OX + 40]].ravel(), minlength=4))

win = maps[PI, OY:OY + 30, OX:OX + 40]


def assemble(pi, ox, oy, atlas, tiles=None):
    """returns 4x RGB image of the 40x30 window"""
    idx = maps[pi, oy:oy + 30, ox:ox + 40]
    if tiles is None:                        # nearest 4x of the original tiles
        out = np.zeros((480, 640, 3), np.uint8)
        for ty in range(30):
            for tx in range(40):
                out[ty * 16:(ty + 1) * 16, tx * 16:(tx + 1) * 16] = PAL[orig[atlas][idx[ty, tx]]]
        return np.repeat(np.repeat(out, S, 0), S, 1)
    out = np.zeros((480 * S, 640 * S, 3), np.uint8)
    for ty in range(30):
        for tx in range(40):
            out[ty * TS:(ty + 1) * TS, tx * TS:(tx + 1) * TS] = tiles[atlas][idx[ty, tx]]
    return out


def assemble_full(pi):
    a = atlas_of(pi)
    out = np.zeros((96 * 16, 96 * 16, 3), np.uint8)
    for ty in range(96):
        for tx in range(96):
            out[ty * 16:(ty + 1) * 16, tx * 16:(tx + 1) * 16] = PAL[orig[a][maps[pi][ty, tx]]]
    return out


wrong = assemble(PI, OX, OY, 0)                       # bug: fixed atlas 000
right = assemble(PI, OX, OY, A)                       # game behaviour
src = assemble(PI, OX, OY, A)                         # 4x nearest reference
hq = assemble(PI, OX, OY, A, hd)

# xBR on the native-resolution scene
nat = np.zeros((480, 640, 3), np.uint8)
for ty in range(30):
    for tx in range(40):
        nat[ty * 16:(ty + 1) * 16, tx * 16:(tx + 1) * 16] = PAL[orig[A][win[ty, tx]]]
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "src.png"
    Image.fromarray(nat).save(p)
    q = Path(td) / "xbr.png"
    r = subprocess.run([r"C:\Program Files\ffmpeg\bin\ffmpeg.exe", "-hide_banner", "-loglevel", "error", "-y",
                        "-i", str(p), "-vf", "xbr=n=4", "-frames:v", "1", str(q)], capture_output=True)
    if r.returncode == 0 and q.exists():
        xb = np.array(Image.open(q).convert("RGB"))
        if xb.shape[:2] != (480 * S, 640 * S):
            xb = np.repeat(np.repeat(xb, S, 0), S, 1)
    else:
        xb = src
        print("xbr failed", r.stderr.decode()[:200])

# native-resolution pixel crop (1:1 at 4x = 16x zoom of the source pixels)
DX, DY = 12, 9                     # tiles
CW, CH = 135, 105                  # source pixels -> 540 x 420 at 4x, shown 1:1

variants = [("① 最近邻 4x（当前游戏画面基线）", src),
            ("② hqx 4x（磁盘上的 HD 素材）", hq),
            ("③ xBR 4x（通用超分）", xb)]

# ---------------------------------------------------------------- sheet
W, M = 1700, 20
F = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 22)
FB = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 30)
FS = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 17)

y = M + 42 + 34                                                # title block
s1 = 30 + 24 + 480 + 24                                        # header + panels + gap
s2 = 28 + 512 + 26
s3 = 28 + 420 + 26
H = y + s1 + s2 + s3 + 34 + M

sheet = Image.new("RGB", (W, H), (16, 18, 22))
d = ImageDraw.Draw(sheet)

d.text((M, M), "港口地图根因排查：atlas 选择错误", font=FB, fill=(240, 240, 245))
d.text((M, M + 42), "PORTMAP 的 tile 编号必须配 CHIP_NO.DAT 指定的 PORTCHIP atlas（本图 port %d → atlas %d）；"
       "固定用 atlas 000 就会拼出左边那种错乱画面。" % (PI, A),
       font=FS, fill=(150, 158, 172))

d.text((M, y), "根因对照：同一张地图、同一个 40×30 窗口，1:1 原生像素", font=F, fill=(230, 232, 238))
y += 30
d.text((M, y), "×  错误：一律使用 atlas 000（旧对比样张的做法）", font=FS, fill=(255, 150, 150))
sheet.paste(Image.fromarray(wrong[::S, ::S]), (M, y + 24))
d.text((M + 660, y), "✓  正确：按 chip_no 选择 atlas %03d" % (A * 2), font=FS, fill=(150, 255, 170))
sheet.paste(Image.fromarray(right[::S, ::S]), (M + 660, y + 24))
y += 24 + 480 + 24

d.text((M, y), "按正确 atlas 拼装的完整港口地图（96×96 tile，1/3 原生像素）", font=F, fill=(230, 232, 238))
y += 28
for i, pi in enumerate([0, 42, 97]):
    full = assemble_full(pi)
    im = Image.fromarray(full).resize((512, 512), Image.Resampling.NEAREST)
    cx = M + i * 540
    sheet.paste(im, (cx, y))
    d.text((cx + 6, y + 6), f"port {pi}  atlas {atlas_of(pi)}", font=FS, fill=(255, 255, 255))
y += 512 + 26

d.text((M, y), "放大策略对照：同一块 %dx%d 源区域，1:1 原生像素（不做任何缩放）" % (CW, CH),
       font=F, fill=(230, 232, 238))
y += 28
for i, (name, img) in enumerate(variants):
    cx = M + i * 560
    crop = img[DY * TS:DY * TS + CH * S, DX * TS:DX * TS + CW * S]
    sheet.paste(Image.fromarray(crop), (cx, y))
    d.text((cx, y + 428), name, font=FS, fill=(210, 216, 228))
y += 420 + 26 + 26

d.text((M, y), "结论：①–③ 在这批 16 色 1px 抖动纹理上几乎没有可见差异——放大算法无法创造细节；要真正变清晰只能重绘。",
       font=FS, fill=(150, 158, 172))

out = ROOT / "docs" / "port-tile-hd-compare.png"
out.parent.mkdir(parents=True, exist_ok=True)
sheet.save(out, optimize=True)
print("wrote", out, sheet.size)

# ================================================================ detail gallery
# hqx vs xBR on the different kinds of map elements; nearest neighbour dropped.
TW, TH = 8, 6                      # tiles per window -> 128 x 96 source px

# The tile classes above are a crude proxy; for the gallery we pick windows by the
# actual palette colour families, so "海洋 / 沙滩 / 草地" really contain those pixels.
FAM = {"water": [3, 8, 9, 10, 11], "sand": [5, 6, 13, 14, 15], "green": [2, 7]}
SHARE = np.zeros((len(orig), 240, 3), np.float32)          # per tile: water/sand/green
for a in range(len(orig)):
    px = orig[a].reshape(240, 256).astype(int)
    for f, fam in enumerate(FAM.values()):
        SHARE[a, :, f] = np.isin(px, fam).mean(1)
NAT = SHARE.sum(-1)                                        # natural (non-man-made) share

ROWS = [("海洋 / 水面：2 色抖动", "water"),
        ("沙滩 / 平地：大块平涂", "sand"),
        ("草地 / 树林：1px 纹理", "green"),
        ("建筑 / 街道：硬边缘", "built"),
        ("海岸线：水陆交界", "coast")]

chosen = []
for label, kind in ROWS:
    cand = None
    for pi in range(len(maps)):
        a = atlas_of(pi)
        g = CLS[a][maps[pi]]
        sh = SHARE[a][maps[pi]]                            # TH*TW windowed below
        for oy in range(0, 96 - TH + 1, 2):
            for ox in range(0, 96 - TW + 1, 2):
                w = sh[oy:oy + TH, ox:ox + TW]
                wcls = g[oy:oy + TH, ox:ox + TW]
                water = float(w[:, :, 0].mean())
                sand = float(w[:, :, 1].mean())
                green = float(w[:, :, 2].mean())
                structural = int((wcls == 3).sum())
                if kind == "water":
                    s = water * 100 - abs(water - 1.0) * 4 - structural * 3
                elif kind == "sand":
                    s = sand * 100 + int((wcls == 1).sum()) * 1.5 - water * 80 - green * 40
                elif kind == "green":
                    s = green * 100 - water * 30 - structural * 4
                elif kind == "built":
                    s = structural * 6 + (1 - float(w.sum(-1).mean())) * 120
                else:                                     # coast: half water, half land
                    mixed = float(((w[:, :, 0] > 0.1) & (w[:, :, 0] < 0.9)).mean())
                    s = mixed * 100 + (1 - abs(water - 0.5)) * 40 - structural * 2
                if any(q[1] == pi and abs(q[2] - ox) < TW and abs(q[3] - oy) < TH for q in chosen):
                    continue
                if cand is None or s > cand[0]:
                    cand = (s, pi, ox, oy)
    chosen.append((label, cand[1], cand[2], cand[3]))
for label, pi, ox, oy in chosen:
    a = atlas_of(pi)
    mix = np.bincount(CLS[a][maps[pi][oy:oy + TH, ox:ox + TW]].ravel(), minlength=4)
    w = SHARE[a][maps[pi][oy:oy + TH, ox:ox + TW]]
    print(f"  {label}: port {pi} atlas {a} @({ox},{oy}) tiles d/f/t/s={mix.tolist()} "
          f"water={w[:, :, 0].mean():.0%} sand={w[:, :, 1].mean():.0%} green={w[:, :, 2].mean():.0%}")

FFMPEG = r"C:\Program Files\ffmpeg\bin\ffmpeg.exe"


def upscale4(method, arr):
    """hqx / xbr on the whole window (no per-tile borders)."""
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "in.png"
        q = Path(td) / "out.png"
        Image.fromarray(arr).save(p)
        r = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
                            "-i", str(p), "-vf", f"{method}=4", "-frames:v", "1", str(q)],
                           capture_output=True)
        if r.returncode == 0 and q.exists():
            return np.array(Image.open(q).convert("RGB"))
        print(f"{method} failed:", r.stderr.decode()[:200])
        return np.repeat(np.repeat(arr, S, 0), S, 1)


def native_win(pi, ox, oy):
    a = atlas_of(pi)
    idx = maps[pi, oy:oy + TH, ox:ox + TW]
    o = np.zeros((TH * 16, TW * 16, 3), np.uint8)
    for ty in range(TH):
        for tx in range(TW):
            o[ty * 16:(ty + 1) * 16, tx * 16:(tx + 1) * 16] = PAL[orig[a][idx[ty, tx]]]
    return o


def hq_win(pi, ox, oy):
    """what the shipped HD tiles give: hqx run on each 16x16 tile in isolation."""
    a = atlas_of(pi)
    idx = maps[pi, oy:oy + TH, ox:ox + TW]
    o = np.zeros((TH * TS, TW * TS, 3), np.uint8)
    for ty in range(TH):
        for tx in range(TW):
            o[ty * TS:(ty + 1) * TS, tx * TS:(tx + 1) * TS] = hd[a][idx[ty, tx]]
    return o


def pct(a, b):
    return float((np.abs(a.astype(int) - b.astype(int)).sum(-1) > 0).mean())


rows = []
for label, pi, ox, oy in chosen:
    nat = native_win(pi, ox, oy)
    tile = hq_win(pi, ox, oy)
    whole = upscale4("hqx", nat)
    xbr = upscale4("xbr", nat)
    d = (pct(tile, whole), pct(whole, xbr), pct(tile, xbr))
    rows.append((label, pi, ox, oy, nat, tile, whole, xbr, d))
    print(f"  {label}: 逐tile↔整窗 {d[0]:.1%}  整窗↔xBR {d[1]:.1%}  逐tile↔xBR {d[2]:.1%}")

C0, C1, C2, C3 = 20, 270, 798, 1326
ROWH = 424
GY = 20 + 42 + 30
GH = GY + 30 + len(rows) * ROWH + 44 + 20
g = Image.new("RGB", (C3 + 512 + 20, GH), (16, 18, 22))
dg = ImageDraw.Draw(g)

dg.text((C0, 20), "港口 tile 放大细节对照：hqx 逐 tile vs hqx 整窗 vs xBR", font=FB, fill=(240, 240, 245))
dg.text((C0, 20 + 42), "全部按 1:1 原生像素显示（每 4×4 方块 = 1 个源像素）。最左是 1:1 源像素缩略图，用来辨认画面内容。",
        font=FS, fill=(150, 158, 172))

cy = GY
dg.text((C0, cy), "源像素", font=FS, fill=(190, 196, 208))
dg.text((C1, cy), "① hqx 逐 tile（磁盘现有 HD 素材）", font=FS, fill=(190, 196, 208))
dg.text((C2, cy), "② hqx 整窗（消除 tile 接缝）", font=FS, fill=(190, 196, 208))
dg.text((C3, cy), "③ xBR 整窗", font=FS, fill=(190, 196, 208))
cy += 30

for label, pi, ox, oy, nat, tile, whole, xbr, d in rows:
    dg.text((C0, cy), label, font=F, fill=(232, 234, 240))
    dg.text((C0, cy + 28), f"port {pi} · atlas {atlas_of(pi)}", font=FS, fill=(140, 148, 162))
    dg.text((C0, cy + 52), f"逐tile↔整窗 {d[0]:.1%}", font=FS, fill=(140, 148, 162))
    dg.text((C0, cy + 72), f"整窗↔xBR {d[1]:.1%}", font=FS, fill=(140, 148, 162))
    dg.text((C0, cy + 92), f"逐tile↔xBR {d[2]:.1%}", font=FS, fill=(140, 148, 162))
    g.paste(Image.fromarray(nat), (C0, cy + 124))
    g.paste(Image.fromarray(tile), (C1, cy + 30))
    g.paste(Image.fromarray(whole), (C2, cy + 30))
    g.paste(Image.fromarray(xbr), (C3, cy + 30))
    cy += ROWH

dg.text((C0, cy + 4), "①②③ 都把 1px 抖动抹成了 4px 色块，无法创造出原图没有的细节；差别只在边缘："
        "xBR 会插出中间色、边缘更软，hqx 基本保留原色板。逐 tile 处理还会在 tile 边界留下 4px 硬接缝。",
        font=FS, fill=(150, 158, 172))

gout = ROOT / "docs" / "port-tile-hd-gallery.png"
gout.parent.mkdir(parents=True, exist_ok=True)
g.save(gout, optimize=True)
print("wrote", gout, g.size)
