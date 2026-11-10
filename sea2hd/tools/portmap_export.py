"""导出一个港口的整图素材，作为生成式重绘的输入。

产出（docs/materials/）：
  port{P}_native.png   1536x1536  原生分辨率（96x96 个 16x16 tile）RGB，日间调色
  port{P}_x4.png       6144x6144  4x 最近邻（给需要更大输入的工具）
  port{P}_manifest.json           回灌所需的全部对齐信息

用法：python portmap_export.py [港号，默认 42]
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(r"d:\OldGames\大航海时代2")
sys.path.insert(0, str(ROOT / "tools"))
from koei_assets import LS11Archive, decode_planar, PORTCHIP_PALETTES

OUT = ROOT / "docs" / "materials"
OUT.mkdir(parents=True, exist_ok=True)
PAL = np.array(PORTCHIP_PALETTES["day"], np.uint8)

PI = int(sys.argv[1]) if len(sys.argv) > 1 else 42

chip_no = list((ROOT / "sea2" / "CHIP_NO.DAT").read_bytes())
arc = LS11Archive(ROOT / "sea2" / "PORTCHIP.LZW")
entry_ids, tiles = [], []
for e in arc.entries:
    raw = arc.decompress(e)
    if len(raw) == 30720:
        entry_ids.append(e.index)
        tiles.append(np.array(decode_planar(raw, 16, 240 * 16, 32, 4), np.uint8).reshape(240, 16, 16))
tiles = np.array(tiles)
marc = LS11Archive(ROOT / "sea2" / "PORTMAP.LZW")
maps = [np.frombuffer(marc.decompress(e), np.uint8).reshape(96, 96)
        for e in marc.entries if e.uncomp_size == 9216]

a = chip_no[PI] if PI < len(chip_no) and chip_no[PI] < len(tiles) else 0
m = maps[PI]

# 原生分辨率整图：每格 16x16，按 CHIP_NO 指定的 atlas 取 tile
native = PAL[tiles[a][m]].transpose(0, 2, 1, 3, 4).reshape(96 * 16, 96 * 16, 3)
Image.fromarray(native).save(OUT / f"port{PI}_native.png", optimize=True)
print(f"port{PI}_native.png {native.shape[1]}x{native.shape[0]} atlas {a}")

x4 = Image.fromarray(native).resize((native.shape[1] * 4, native.shape[0] * 4), Image.Resampling.NEAREST)
x4.save(OUT / f"port{PI}_x4.png", optimize=True)
print(f"port{PI}_x4.png {x4.size}")

# 回灌信息：重绘后的图必须与这张网格严格对齐，才知道每个 tile 从哪里取样
used = sorted({int(t) for t in np.unique(m)})
manifest = {
    "port": PI,
    "atlas": int(a),
    "atlas_entry": int(entry_ids[a]),
    "palette": "day",
    "map_size_tiles": [96, 96],
    "tile_px_native": 16,
    "native_size": [1536, 1536],
    "x4_size": [6144, 6144],
    "tiles_used": used,
    "note": "重绘输出必须仍是 1536x1536 的同一网格；任意 tile (tx,ty) 的像素块为 "
            "x=[16*tx,16*tx+16), y=[16*ty,16*ty+16)。4x 显示块为 x=[64*tx,64*tx+64)。",
}
(OUT / f"port{PI}_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1),
                                             encoding="utf-8")
print(f"port{PI}_manifest.json（用到 {len(used)} 种 tile）")
