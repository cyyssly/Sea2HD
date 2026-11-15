"""港口符号 tile 提取与高清图标库。

港口的 1536x1536 整图由 PORTCHIP 的 16x16 图块拼成，其中少数图块不是地貌纹理，
而是画在地面上的“符号”: 建筑门口的招牌、井、树、十字、天平之类。这些图块一旦被
生成式重绘改动，画面上的标志物就与原版不一致了，因此需要单独认出来并锁成
“原样的 4x 最近邻放大”。

识别以“图样”为单位，而不是单个图块：同一张像素图在不同气候的图集里就是同一个
标志物，只是放在不同槽位。判定用三个与调色板无关的客观信号：

* **跨图集复现** —— 同一图样在 7 套图集里至少出现 5 套。地貌/建材纹理只跨 3~4 套，
  换个气候就变了；标志物则是全世界通用的贴图。这是最强的判别信号。
* **稀有度** —— 每张地图里的平均出现次数 ≤ 2.2。地貌动辄几十上百次；招牌每张地图
  才一两个。按“每张地图”而不是总数统计，才不会因为各套图集的地图数不同而失真。
* **贴纸结构** —— 背景色（占比最高的颜色）必须是调色板索引 0（黑），且有一个被背景
  整个包住的连通块。空白块、平涂渐变、沙滩过渡都没有这个结构。

被判定为标志物的图样，它在所有图集里的实例都算符号（同一张图在哪里都是标志物）。
图集独有的个别标志物（例如只有两个港口用的盾牌、十字）用
`docs/materials/symbols-overrides.json` 手工补入。

输出：
    docs/materials/symbols.json          规范图清单、槽位映射表与每套图集的指标
    docs/materials/symbols-ref.png       规范图核对表（人工检查用）
    assets/hd/PORTCHIP/symbols/sym-NN.png   规范图，4x 最近邻（64x64）

同一张图在 5~7 套图集里各有一份（槽位号不同），按像素内容合并成一张规范图；地图里存的
槽位号用 symbols.json 的 map（"000/083" -> "sym-01"）找回对应的规范图。

用法：
    python port_symbols.py            生成全部输出
    python port_symbols.py --dry-run  只打印统计，不写文件
"""
import hashlib
import json
import sys
from collections import defaultdict, deque
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
from koei_assets import LS11Archive, PORTCHIP_PALETTES, decode_planar

MAT = ROOT / "docs" / "materials"
MAT.mkdir(parents=True, exist_ok=True)
LIB = ROOT / "assets" / "hd" / "PORTCHIP" / "symbols"
OVERRIDES = MAT / "symbols-overrides.json"

# ---------------------------------------------------------------- 判定门槛
# 判定以“图样”为单位：同一张像素图在不同图集里是同一个标志物，逐实例看反而会漏。
SharedAtlases = 5     # 同一图样至少出现在这么多套图集里
SharedRate = 2.2      # 而且每张地图里的平均出现次数不超过这个值
DarkBackground = 0    # 背景（占比最高的颜色）必须是调色板索引 0（黑）
MinColours = 2        # 排除空白/纯色图块
MinBlob = 1           # 必须有一个被背景包住的连通块（排除平涂渐变）

BORDER = ([(0, j) for j in range(16)] + [(15, j) for j in range(16)] +
          [(i, 0) for i in range(1, 15)] + [(i, 15) for i in range(1, 15)])
BY, BX = zip(*BORDER)

# ---------------------------------------------------------------- 原始数据
chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, orig = [], []
for e in arc.entries:
    raw = arc.decompress(e)
    if len(raw) == 30720:
        entry_ids.append(e.index)
        orig.append(np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8)
                    .reshape(240, 16, 16))
orig = np.array(orig)
NA = len(orig)

# 索引号本身只是调色板下标，出图和判图都要换成日间调色板的真实颜色。
PAL = np.array(PORTCHIP_PALETTES["day"], np.uint8)
COLOUR = PAL[orig]

marc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = np.array([np.frombuffer(marc.decompress(e), np.uint8).reshape(96, 96)
                 for e in marc.entries if e.uncomp_size == 9216])
atlas_of = np.array([chip_no[i] if i < len(chip_no) and chip_no[i] < NA else 0
                     for i in range(len(maps))])

usage = np.zeros((NA, 240), np.int32)
maxpp = np.zeros((NA, 240), np.int32)
for m, a in zip(maps, atlas_of):
    c = np.bincount(m.ravel(), minlength=256)[:240]
    usage[a] += c
    maxpp[a] = np.maximum(maxpp[a], c)


# ---------------------------------------------------------------- 形状特征
def features(px):
    """背景色、背景占比、边框纯度、以及“被背景包住的最大连通块”。"""
    vals, cnt = np.unique(px, return_counts=True)
    bg = int(vals[np.argmax(cnt)])
    border = float(np.mean(px[list(BY), list(BX)] == bg))
    other = px != bg
    # 从边框往里走，凡是连到边框的非背景像素都属于“背景上的东西”，
    # 剩下的才是被背景整个围住的物体。
    seen = np.zeros((16, 16), bool)
    dq = deque()
    for y, x in BORDER:
        if other[y, x] and not seen[y, x]:
            seen[y, x] = True
            dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < 16 and 0 <= nx < 16 and other[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                dq.append((ny, nx))
    rest = other & ~seen
    big, fill = 0, 0.0
    lab = np.zeros((16, 16), bool)
    for y in range(16):
        for x in range(16):
            if not (rest[y, x] and not lab[y, x]):
                continue
            q = [(y, x)]
            lab[y, x] = True
            pts = []
            while q:
                cy, cx = q.pop()
                pts.append((cy, cx))
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < 16 and 0 <= nx < 16 and rest[ny, nx] and not lab[ny, nx]:
                        lab[ny, nx] = True
                        q.append((ny, nx))
            if len(pts) > big:
                ys = [p[0] for p in pts]
                xs = [p[1] for p in pts]
                big = len(pts)
                fill = big / float((max(ys) - min(ys) + 1) * (max(xs) - min(xs) + 1))
    return bg, border, int(rest.sum()), big, fill, int(len(vals))


FEAT = {}
for a in range(NA):
    for t in range(240):
        if usage[a, t]:
            FEAT[(a, t)] = features(orig[a, t])

# ---------------------------------------------------------------- 跨图集复现
groups = defaultdict(list)
for (a, t) in FEAT:
    groups[hashlib.sha1(orig[a, t].tobytes()).hexdigest()].append((a, t))

# ---------------------------------------------------------------- 选型
# 每套图集分到多少张地图差别很大（4 张到 20 多张），所以稀有度要按“每张地图的
# 平均出现次数”算，不能直接用总数。
maps_per_atlas = np.array([int((atlas_of == a).sum()) for a in range(NA)])


def qualifies(members):
    """整族一起判定：只要这张图样够通用、够稀有、结构像贴纸，它在各图集的实例都算符号。"""
    if len({a for a, _ in members}) < SharedAtlases:
        return False
    a0, t0 = members[0]
    bg, _border, _rest, big, _fill, ncol = FEAT[(a0, t0)]
    if bg != DarkBackground or ncol < MinColours or big < MinBlob:
        return False
    return max(usage[a, t] / max(1, maps_per_atlas[a]) for a, t in members) <= SharedRate


over = {"add": {}, "remove": {}}
if OVERRIDES.exists():
    over = json.loads(OVERRIDES.read_text(encoding="utf-8"))
    over.setdefault("add", {})
    over.setdefault("remove", {})

picked = {}
for digest, members in groups.items():
    if qualifies(members):
        for a, t in members:
            picked[(a, t)] = "shared"
for key, tiles in over["add"].items():
    for t in tiles:
        # 没有被任何地图用过的图块不在 FEAT 里，后面取指标会 KeyError。
        if (int(key), int(t)) in FEAT:
            picked[(int(key), int(t))] = "manual"
for key, tiles in over["remove"].items():
    for t in tiles:
        picked.pop((int(key), int(t)), None)

nfam = sum(1 for m in groups.values() if qualifies(m))
print(f"图集 {NA} 套，地图 {len(maps)} 张，符号图块 {len(picked)} 个"
      f"（其中通用图样 {nfam} 族）")
for a in range(NA):
    mine = sorted(t for (aa, t) in picked if aa == a)
    print(f"  atlas {a} entry {entry_ids[a]:03d} maps={int((atlas_of == a).sum()):2d} "
          f"{len(mine):2d}: " + " ".join(f"{t:03d}" for t in mine))

if "--dry-run" in sys.argv:
    raise SystemExit(0)

# ---------------------------------------------------------------- 规范图
# 同一张图案在 5~7 套图集里各占一个槽位，像素完全相同。按像素合并，只留一张规范图，
# 槽位与规范图的对应关系写进映射表。
families = sorted(([m for m in members if m in picked] for digest, members in groups.items()),
                  key=lambda inst: min(inst) if inst else (99, 99))
families = [inst for inst in families if inst]
canon_id = {}
canon = []
for i, inst in enumerate(families, 1):
    cidn = f"sym-{i:02d}"
    for m in inst:
        canon_id[m] = cidn
    canon.append({"id": cidn,
                  "pixel_sha1": hashlib.sha1(orig[inst[0]].tobytes()).hexdigest(),
                  "instances": [f"{entry_ids[a]:03d}/{t:03d}" for a, t in inst]})
slot = {f"{entry_ids[a]:03d}/{t:03d}": canon_id[(a, t)] for a, t in sorted(picked)}

# ---------------------------------------------------------------- 高清图标库
LIB.mkdir(parents=True, exist_ok=True)
# 先清空旧的规范图，避免规则变动后留下已经不算符号的图块。
# 只匹配 sym-NN.png：同目录下人工重绘的 sym-NN_hd.png 必须保留。
for old in LIB.glob("sym-??.png"):
    old.unlink()
for inst in families:
    a, t = inst[0]
    # 必须是 4x 最近邻：它代表“这里什么都不改”，像素放大后与运行时输出逐像素相同。
    Image.fromarray(COLOUR[a, t]).resize((64, 64), Image.NEAREST).save(
        LIB / f"{canon_id[(a, t)]}.png", optimize=True)

# ---------------------------------------------------------------- 核对表
COLS, PITCH, TILE, MARGIN = 2, 404, 64, 20
F = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 12)
FT = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 18)
rows = (len(families) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * PITCH + 2 * MARGIN, rows * (TILE + 32) + 2 * MARGIN + 30),
                  (56, 58, 64))
d = ImageDraw.Draw(sheet)
d.text((MARGIN, MARGIN), f"符号规范图 {len(families)} 张 · {len(picked)} 个槽位 · 4x 最近邻",
       font=FT, fill=(238, 240, 246))
for i, inst in enumerate(families):
    a, t = inst[0]
    cidn = canon_id[(a, t)]
    r, c = divmod(i, COLS)
    x, y = MARGIN + c * PITCH, MARGIN + 34 + r * (TILE + 32)
    sheet.paste(Image.fromarray(COLOUR[a, t]).resize((TILE, TILE), Image.NEAREST), (x, y))
    names = [f"{entry_ids[b]:03d}/{u:03d}" for b, u in inst]
    d.text((x + TILE + 10, y + 4), f"{cidn}  ×{len(inst)}", font=FT, fill=(238, 240, 246))
    for k in range(0, len(names), 4):
        d.text((x + TILE + 10, y + 32 + k // 4 * 16), "  ".join(names[k:k + 4]),
               font=F, fill=(200, 206, 220))
sheet.save(MAT / "symbols-ref.png", optimize=True)

# ---------------------------------------------------------------- 清单
manifest = {
    "note": "符号图块 = 画在地面上的标志物，重绘时必须逐像素保持不变。",
    "rule": {"shared_atlases": SharedAtlases, "shared_rate": SharedRate,
             "dark_background": DarkBackground, "min_colours": MinColours,
             "min_blob": MinBlob},
    "library": "assets/hd/PORTCHIP/symbols/{id}.png",
    "hd_library": "assets/hd/PORTCHIP/symbols/{id}_hd.png",
    "hd_size": 64,
    "map": slot,
    "canonical": canon,
    "atlases": [],
}
for a in range(NA):
    cells = []
    for t in sorted(t for (aa, t) in picked if aa == a):
        bg, border, rest, big, fill, ncol = FEAT[(a, t)]
        cells.append({
            "tile": int(t), "symbol": canon_id[(a, t)], "reason": picked[(a, t)],
            "usage": int(usage[a, t]), "max_per_map": int(maxpp[a, t]),
            "colours": ncol, "border": round(border, 3), "blob": big,
            "blob_fill": round(fill, 3), "background_index": bg,
        })
    manifest["atlases"].append({"atlas": a, "entry": int(entry_ids[a]),
                                "maps": int((atlas_of == a).sum()), "symbols": cells})
(MAT / "symbols.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1),
                                  encoding="utf-8")
print(f"输出：{MAT / 'symbols.json'}")
print(f"      {MAT / 'symbols-ref.png'}")
print(f"      {LIB}（{len(families)} 张规范图，覆盖 {len(picked)} 个槽位）")
