"""最小验证：把水/沙/草三类材质替换为「程序生成的无缝贴图」，建筑保持原 tile。

不涉及 AI、不涉及整图、不涉及视口——只为验证两件事：
  1. 同类材质共用贴图后，tile 边界是否真的无缝（数值 + 目视）
  2. 换掉材质后，整张港口地图是否还能读懂（建筑、道路、海岸线还在原位）

输出：docs/materials/material-swap-test.png
"""
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
S = 4                      # 每个 tile 放大到 64x64

chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, tiles = [], []
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

WATER = [3, 8, 9, 10, 11]
SAND = [5, 6, 13, 14, 15]
GREEN = [2, 7]

# ---------------------------------------------------------------- 每格材质分类
# 判据：整格有 80% 以上的像素落在同一颜色族（水/沙/绿），且没有明显物件。
# 这类格子是「可平铺的底材」，换掉只损失花纹，不损失建筑/道路轮廓。
FAM = np.zeros((NA, 240, 3), np.float32)
for a in range(NA):
    px = tiles[a].reshape(240, 256)
    for f, fam in enumerate([WATER, SAND, GREEN]):
        FAM[a, :, f] = np.isin(px, fam).mean(1)
MAT = np.where(FAM.max(2) >= 0.8, FAM.argmax(2), 3).astype(np.uint8)     # 0水 1沙 2绿 3保留
for a in range(NA):
    print(f"atlas {a}: 水 {int((MAT[a] == 0).sum())} 沙 {int((MAT[a] == 1).sum())} "
          f"草 {int((MAT[a] == 2).sum())} 保留 {int((MAT[a] == 3).sum())}")

# ---------------------------------------------------------------- 无缝材质场
# 关键：所有同材质格子必须从「同一张周期场」按世界坐标采样。
# 任意两张 64 周期贴图平移后拼在一起都会断缝（roll(dx) 只在 dx 相同时连续），
# 所以不能用「每格随机相位变体」。改为生成一张 P=256 的周期场，按绝对
# 世界坐标 (tx*64+x, ty*64+y) % P 取样：跨格处取到的就是相邻像素，天然连续。
# 代价是图案每 256 像素（即 4 个 tile）在整张图上重复一次。
P = 256                    # 场周期，必须能被 6144 整除（6144 = 256 * 24）


def value_noise(seed, cells):
    rng = np.random.default_rng(seed)
    g = rng.random((cells, cells)).astype(np.float32)
    x = np.arange(P) * cells / P
    i0 = np.floor(x).astype(int) % cells
    i1 = (i0 + 1) % cells
    w = (x - np.floor(x)).astype(np.float32)
    a = g[i0][:, i0]
    b = g[i0][:, i1]
    c = g[i1][:, i0]
    d = g[i1][:, i1]
    wy = w[:, None]
    wx = w[None, :]
    return (a * (1 - wy) * (1 - wx) + b * (1 - wy) * wx + c * wy * (1 - wx) + d * wy * wx)


def fbm(seed, octaves=((4, .55), (8, .3), (16, .15), (32, .1))):
    s = np.zeros((P, P), np.float32)
    for i, (cells, amp) in enumerate(octaves):
        s += value_noise(seed + i, cells) * amp
    lo, hi = s.min(), s.max()
    return (s - lo) / (hi - lo + 1e-6)


def ramp(idxs):
    """该颜色族按亮度排序后的调色板色阶；新贴图只用游戏原本的这几种颜色。"""
    ids = np.array(idxs)
    ids = ids[np.argsort(PAL[ids].astype(int).sum(1))]
    return PAL[ids].astype(np.float32)


def water_tex():
    y, x = np.mgrid[0:P, 0:P].astype(np.float32)
    wave = 0.5 + 0.5 * np.sin(2 * np.pi * (x * 4 / P) + 2 * np.pi * (y * 2 / P))     # 4/2 个整周期 -> 无缝
    n = fbm(11, ((8, .5), (16, .3), (32, .2)))
    return ramp(WATER), 0.55 * wave + 0.45 * n


def sand_tex():
    return ramp(SAND), fbm(22, ((8, .4), (16, .35), (32, .25)))


def grass_tex():
    f = 0.5 * fbm(33, ((8, .5), (16, .3), (32, .2))) + 0.5 * fbm(34, ((64, .7), (32, .3)))
    return ramp(GREEN), f


def bake(pal, f, levels):
    i = np.clip((f * levels).astype(int), 0, levels - 1)
    return pal[i]


TEX = [(lambda p, f: bake(p, f, len(p)))(*tex()) for tex in (water_tex, sand_tex, grass_tex)]

# 自检：场按世界坐标平铺后是否连续（取一个跨越周期的接缝）
for c in range(3):
    t = TEX[c]
    print(f"材质 {c} 场环绕差异：左右 {np.abs(t[:, 0].astype(int) - t[:, -1].astype(int)).mean():.2f}，"
          f"上下 {np.abs(t[0].astype(int) - t[-1].astype(int)).mean():.2f}")

# ---------------------------------------------------------------- 高清 tile（建筑等保留原样用 hqx 素材）
hd = {}
for a, eid in enumerate(entry_ids):
    for t in range(240):
        hd[(a, t)] = np.array(Image.open(ROOT / "assets" / "hd" / "PORTCHIP" / "tiles" /
                                       f"{eid:03d}_{t:03d}.png").convert("RGB"), np.uint8)


FULL = 96 * 16 * S          # 整图 4x 边长 = 6144


def render(pi, swap):
    a = atlas_of[pi]
    m = maps[pi]
    out = np.zeros((FULL, FULL, 3), np.uint8)
    for ty in range(96):
        for tx in range(96):
            t = int(m[ty, tx])
            y, x = ty * 16 * S, tx * 16 * S
            if swap and MAT[a, t] < 3:
                continue                       # 材质格先留空，稍后整场填充
            if swap:
                out[y:y + 64, x:x + 64] = hd[(a, t)]
            else:
                out[y:y + 64, x:x + 64] = np.repeat(np.repeat(PAL[tiles[a, t]], S, 0), S, 1)
    if swap:
        mc = MAT[a][m]
        for c in range(3):
            mask_t = (mc == c)
            if not mask_t.any():
                continue
            mask = np.repeat(np.repeat(mask_t, 64, axis=0), 64, axis=1)
            field = np.tile(TEX[c], (FULL // P, FULL // P, 1))
            out[mask] = field[mask]
    return out


PI = 42
o1 = render(PI, False)
o2 = render(PI, True)

# ---------------------------------------------------------------- 接缝度量
a = atlas_of[PI]
m = maps[PI]
mc = MAT[a][m]
xs = 64 * np.arange(1, 96)
ys = 64 * np.arange(1, 96)

# 只统计「两侧都是可平铺材质」的边界；材质与建筑之间本来就该有落差
sel_h = np.repeat((mc[:, :-1] == mc[:, 1:]) & (mc[:, :-1] < 3), 64, axis=0)
sel_v = np.repeat((mc[:-1, :] == mc[1:, :]) & (mc[:-1, :] < 3), 64, axis=1)

# 格内落差用稀疏采样（整幅逐像素点差太占内存，采样足以给出量级）
rs = np.arange(0, FULL, 7)
cs = np.arange(1, FULL, 7)
cs = cs[~np.isin(cs, xs)]
rs = rs[~np.isin(rs, ys)]


def seam_stats(img):
    dh = np.abs(img[:, xs].astype(np.int16) - img[:, xs - 1].astype(np.int16)).sum(2)
    dv = np.abs(img[ys].astype(np.int16) - img[ys - 1].astype(np.int16)).sum(2)
    px = img[np.ix_(rs, cs)].astype(np.int16)
    pxl = img[np.ix_(rs, cs - 1)].astype(np.int16)
    interior = np.abs(px - pxl).sum(2).mean()
    return (dh[sel_h].mean(), dv[sel_v].mean(), interior)


for name, im in (("原版  ", o1), ("换材质", o2)):
    h, v, ins = seam_stats(im)
    print(f"{name}：材质边界落差 水平 {h:.2f} / 垂直 {v:.2f}   格内落差 {ins:.2f}")

# ---------------------------------------------------------------- 输出图
FS = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 16)
FB = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 24)
Z = 640                                  # 整图预览尺寸

CX, CY, CW, CH = 8, 22, 22, 14           # 4x 细节裁切窗口（tile 坐标），下面自动挑最佳位置
best = None
for oy in range(0, 96 - CH + 1, 2):
    for ox in range(0, 96 - CW + 1, 2):
        w = mc[oy:oy + CH, ox:ox + CW]
        bal = min(int((w < 3).sum()), int((w == 3).sum()))
        if best is None or bal > best[0]:
            best = (bal, ox, oy)
_, CX, CY = best
print(f"细节窗口 ({CX},{CY}) 材质/结构 = {int((mc[CY:CY+CH, CX:CX+CW] < 3).sum())}/"
      f"{int((mc[CY:CY+CH, CX:CX+CW] == 3).sum())}")
PW, PH = CW * 64, CH * 64


def crop(img):
    return img[CY * 64:(CY + CH) * 64, CX * 64:(CX + CW) * 64]


W = max(2 * Z + 3 * 24, 2 * PW + 3 * 24)
H = 60 + Z + 40 + 32 + PH + 30 + 60
img = Image.new("RGB", (W, H), (16, 18, 22))
d = ImageDraw.Draw(img)
d.text((24, 18), f"材质替换最小验证 · port {PI}（水/沙/草换成程序生成无缝贴图，建筑保留原 tile）",
       font=FB, fill=(240, 242, 248))

img.paste(Image.fromarray(o1).resize((Z, Z), Image.Resampling.LANCZOS), (24, 60))
img.paste(Image.fromarray(o2).resize((Z, Z), Image.Resampling.LANCZOS), (24 + Z + 24, 60))
d.text((24, 60 + Z + 8), "原版（真实 tile，1/2.4 缩放）", font=FS, fill=(200, 206, 220))
d.text((24 + Z + 24, 60 + Z + 8), "材质替换后", font=FS, fill=(150, 255, 170))

yy = 60 + Z + 40
d.text((24, yy), f"4x 细节对照（{CW}x{CH} tile，1:1 像素）", font=FB, fill=(240, 242, 248))
yy += 32
img.paste(Image.fromarray(crop(o1)), (24, yy))
img.paste(Image.fromarray(crop(o2)), (24 + PW + 24, yy))
d.text((24, yy + PH + 6), "原版（NN 4x）", font=FS, fill=(200, 206, 220))
d.text((24 + PW + 24, yy + PH + 6), "材质替换（同类格子共用无缝贴图）", font=FS, fill=(150, 255, 170))

out = OUT / "material-swap-test.png"
img.save(out, optimize=True)
print("wrote", out, img.size)
