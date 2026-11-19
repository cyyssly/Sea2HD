"""把人工重绘的符号贴回港口高清图。

港口高清图 `PORTMAP/NNN.png` 是整张 6144x6144 的重绘结果，其中画在地面上的标志物
（门口招牌、井、十字等）也在里面，被生成式重绘改得面目全非。本工具按 tile id 把这些
区块重新覆盖成 `PORTCHIP/symbols/sym-NN.png`（assets/hd 下的人工重绘版），其余像素一律不动。

约定：
    assets/hd/PORTCHIP/symbols/sym-NN.png       人工重绘的符号，64x64（16x16 tile 的 4x）
                                                允许其它尺寸，会按 LANCZOS 缩到 64x64
                                                文件名写成 sym01.png 也认
    assets/original/PORTCHIP/symbols/sym-NN.png 原作基准，工具生成，不要改
    槽位与规范图的对应关系见 docs/materials/symbols.json 的 map

用法：
    python port_symbols_apply.py                列出哪些地图用到已有重绘符号（不写文件）
    python port_symbols_apply.py --map 002      只贴这一张
    python port_symbols_apply.py --all          贴所有已经有高清图的地图

`--map` 指定的地图如果还没有高清图，会用原作按 4x 最近邻铺一张出来，方便先看符号
效果；之后 AI 重绘出 `NNN.png` 覆盖即可。
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def find_root():
    for p in Path(__file__).resolve().parents:
        if (p / "sea2" / "PORTMAP.LZW").exists():
            return p
    raise SystemExit("找不到游戏根目录（sea2/PORTMAP.LZW）")


ROOT = find_root()
sys.path.insert(0, str(ROOT / "tools"))
from koei_assets import LS11Archive, PORTCHIP_PALETTES, decode_planar

MAT = ROOT / "docs" / "materials"
SYM = ROOT / "assets" / "hd" / "PORTCHIP" / "symbols"
ART = ROOT / "assets" / "hd" / "PORTMAP"
GRID = 96           # 一张港口图 96x96 个 tile
STEP = 16           # 原版 tile 边长
SCALE = 4           # HD 倍率
TILE_HD = STEP * SCALE    # 64
SIZE_HD = GRID * TILE_HD  # 6144

PAL = np.array(PORTCHIP_PALETTES["day"], np.uint8)

# ---------------------------------------------------------------- 原始数据
manifest = json.loads((MAT / "symbols.json").read_text(encoding="utf-8"))
slot = {}
for atlas in manifest["atlases"]:
    for cell in atlas["symbols"]:
        slot[(atlas["entry"], cell["tile"])] = cell["symbol"]

chip = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, atlases = [], []
for e in chip.entries:
    raw = chip.decompress(e)
    if len(raw) == 30720:
        entry_ids.append(e.index)
        atlases.append(np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8)
                       .reshape(240, 16, 16))
# 索引号只是调色板下标，铺预览底图时换成日间调色板的颜色。
colour = PAL[np.array(atlases)]

chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = [np.frombuffer(arc.decompress(e), np.uint8).reshape(GRID, GRID)
        for e in arc.entries if e.uncomp_size == GRID * GRID]


def atlas_of(index):
    return chip_no[index] if index < len(chip_no) and chip_no[index] < len(entry_ids) else 0


def entry_of(index):
    return entry_ids[atlas_of(index)]


def original_pixels(index):
    """原作整图 1536x1536，用于还没有高清图时铺预览底。"""
    px = colour[atlas_of(index)][maps[index]]
    return px.transpose(0, 2, 1, 3, 4).reshape(GRID * STEP, GRID * STEP, 3)


# ---------------------------------------------------------------- 重绘符号
icons = {}
for path in sorted(SYM.glob("sym*.png")):
    digits = path.stem[3:].lstrip("-_ ")
    if not digits.isdigit():
        continue
    im = Image.open(path).convert("RGBA")
    if im.size != (TILE_HD, TILE_HD):
        print(f"  {path.name}: {im.width}x{im.height} -> 缩放到 {TILE_HD}x{TILE_HD}")
        im = im.resize((TILE_HD, TILE_HD), Image.LANCZOS)
    else:
        im.load()
    icons[f"sym-{int(digits):02d}"] = im

if not icons:
    print(f"注意：{SYM} 下还没有 sym-NN.png")


def symbols_in(index):
    """这张地图里每个出现过的符号 -> 格子列表。"""
    entry = entry_of(index)
    found = {}
    tiles = maps[index]
    for cy in range(GRID):
        for cx in range(GRID):
            sid = slot.get((entry, int(tiles[cy, cx])))
            if sid:
                found.setdefault(sid, []).append((cx, cy))
    return found


def base_art(index, preview):
    path = ART / f"{index:03d}.png"
    if path.exists() or not preview:
        im = Image.open(path).convert("RGB")
        if im.size != (SIZE_HD, SIZE_HD):
            raise SystemExit(f"{path.name} 尺寸 {im.size}，应为 {SIZE_HD}x{SIZE_HD}")
        return im, path
    src = Image.fromarray(original_pixels(index))
    print(f"  {index:03d} 还没有高清图，用原作 4x 最近邻铺底")
    return src.resize((SIZE_HD, SIZE_HD), Image.NEAREST), path


def apply(index, preview):
    art, path = base_art(index, preview)
    found = symbols_in(index)
    pasted = 0
    for sid, cells in sorted(found.items()):
        icon = icons.get(sid)
        if icon is None:
            continue
        for cx, cy in cells:
            # 整个 64x64 区块都被覆盖：符号在原版里是不透明的地面贴图，不存在透出底色。
            art.paste(icon, (cx * TILE_HD, cy * TILE_HD), icon)
            pasted += 1
    if not pasted:
        print(f"  {index:03d} 没有可用重绘符号，跳过")
        return
    art.save(path)
    print(f"  {index:03d} -> {path.name}：贴回 {pasted} 个区块 "
          f"({', '.join(f'{k}x{len(v)}' for k, v in sorted(found.items()) if k in icons)})")


args = sys.argv[1:]


def opt(name):
    return args[args.index(name) + 1] if name in args else None


want = opt("--map")
if want or "--all" in args:
    if not icons:
        raise SystemExit(f"{SYM} 下没有 sym-NN.png，先放一张重绘的符号再运行")
    if want:
        index = int(want)
        if not 0 <= index < len(maps):
            raise SystemExit(f"没有编号 {index} 的港口地图（共 {len(maps)} 张）")
        apply(index, preview=True)
    else:
        for i in range(len(maps)):
            if (ART / f"{i:03d}.png").exists():
                apply(i, preview=False)
else:
    # 列出每个符号被哪些地图用到，方便挑一张进游戏看效果。
    used = {sid: [] for sid in sorted(set(slot.values()))}
    for i in range(len(maps)):
        for sid, cells in symbols_in(i).items():
            used[sid].append((i, len(cells)))
    for sid, rows in used.items():
        total = sum(n for _, n in rows)
        names = " ".join(f"{i:03d}" for i, _ in rows[:8])
        if len(rows) > 8:
            names += f" …（共 {len(rows)} 张）"
        print(f"  {sid}{' *' if sid in icons else '  '} {total:3d} 个区块：{names}")
    print("（* = 已有重绘文件。确认后加 --map NNN 或 --all 才会写文件）")
