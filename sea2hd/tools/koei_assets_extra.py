"""koei_assets_extra.py — 《大航海时代2》补充美术资源解包（只读原游戏，只写新目录）

与 tools/koei_assets.py 的关系：
  那个工具已经处理 KAO.LZW / PORTCHIP.LZW 的 PNG 导出与回封，本工具**不改动它**，
  只补齐它没覆盖的部分，并且所有产物都写进一个全新的输出目录，绝不覆盖已有资源。

新增覆盖：
  1. LS10 归档：CHAR / IAE1 / IAP1..6 / OPGRAPH。
     实测 LS10 与 LS11 共用同一个 LZ 内核与 256 字节字典，差别只在 FAT 的结束标记：
       LS11 = 12 字节/条，末尾一条 comp_size=0 的 12 字节记录；
       LS10 = 12 字节/条，末尾只有 4 字节 0。
     LS10 条目数由第一条数据的 offset 反推：n = (first_offset - 276) / 12。
  2. 未解包的 LS11 归档：DATA1 / PORTMAP / WORLDMAP（KAO / PORTCHIP 也一并做无损副本，
     但不重复生成它们已有的 PNG）。
  3. PORTMAP 的 101 张 96x96 8bpp 图块（灰度预览；真实 256 色调色板尚未定位）。
  4. 2.PAT：217600 字节，按 32 字节一块切成 6800 块，以 16x16 单色位图
     （每行 2 字节、MSB-first）做可视化。注意：块内容**不是** 1.PAT 那套汉字点阵
     （与 1.PAT 的 5401 个字形零精确匹配、6800 块两两互不相同、无空白块），
     更接近图形 pattern / 符号库，语义未定，只作为「切片可视化 + 无损数据」提供。
  5. DAT 偏移表容器：GRAPH / GRAPH2 / ENDGRP / EVENT0..6，按表无损拆成子块，
     并从块首 4 字节读出小端 width/height（这些块是 RLE/位平面图形，编码未解）。

确定性分级见输出目录的 README.md。所有 .bin 都是逐字节无损的解压结果。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, obj) -> None:
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")


def safe_bytes(path: Path, data: bytes) -> bool:
    """只在文件不存在时写入；返回是否真的写了。"""
    if path.exists():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def safe_png(path: Path, image: Image.Image) -> bool:
    if path.exists():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return True


# --------------------------------------------------------------------------
# KOEI LZ 内核（LS10 / LS11 共用）
# --------------------------------------------------------------------------
class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.bit_position = 0

    def read_bit(self) -> int:
        byte_position = self.bit_position // 8
        if byte_position >= len(self.data):
            raise ValueError("位流意外结束")
        shift = 7 - self.bit_position % 8
        self.bit_position += 1
        return (self.data[byte_position] >> shift) & 1

    def read_code(self) -> int:
        code1 = 0
        count = 0
        while True:
            bit = self.read_bit()
            code1 = (code1 << 1) | bit
            count += 1
            if bit == 0:
                break
        code2 = 0
        for _ in range(count):
            code2 = (code2 << 1) | self.read_bit()
        return code1 + code2


@dataclass(frozen=True)
class Entry:
    index: int
    comp_size: int
    uncomp_size: int
    offset: int


class KoeiArchive:
    """支持 LS10 与 LS11 两种 FAT 布局的只读归档。"""

    def __init__(self, source: Path):
        self.source = source
        self.data = source.read_bytes()
        if len(self.data) < 284 or self.data[:4] not in (b"LS10", b"LS11"):
            raise ValueError(f"{source.name} 不是 LS10/LS11 文件")
        self.kind = self.data[:4].decode("ascii")
        self.dictionary = self.data[16:272]
        self.entries: list[Entry] = []
        self.fat_end = 0
        self.tail_size = 0

        if self.kind == "LS11":
            position = 272
            while position + 12 <= len(self.data):
                comp, uncomp, offset = struct.unpack_from(">III", self.data, position)
                position += 12
                if comp == 0:
                    self.fat_end = position
                    break
                self.entries.append(Entry(len(self.entries), comp, uncomp, offset))
            else:
                raise ValueError("LS11 未找到 FAT 结束标记")
        else:
            # LS10：第一条数据的 offset 位于第一条 FAT 记录的第三字段（272+8）
            first_offset = struct.unpack_from(">I", self.data, 280)[0]
            count = (first_offset - 276) // 12
            if count <= 0 or 276 + count * 12 > len(self.data):
                raise ValueError("LS10 FAT 条目数不合理")
            for i in range(count):
                comp, uncomp, offset = struct.unpack_from(">III", self.data, 272 + i * 12)
                self.entries.append(Entry(i, comp, uncomp, offset))
            self.fat_end = 276 + count * 12
            if self.data[272 + count * 12 : self.fat_end] != b"\0\0\0\0":
                raise ValueError("LS10 FAT 尾部不是 4 字节零标记")

        for e in self.entries:
            if e.offset + e.comp_size > len(self.data):
                raise ValueError(f"条目 {e.index} 数据越界")
        if self.entries:
            last = self.entries[-1]
            self.tail_size = len(self.data) - (last.offset + last.comp_size)

    def decompress(self, entry: Entry) -> bytes:
        compressed = self.data[entry.offset : entry.offset + entry.comp_size]
        if entry.comp_size == entry.uncomp_size:
            return compressed
        reader = BitReader(compressed)
        output = bytearray()
        while len(output) < entry.uncomp_size:
            code = reader.read_code()
            if code < 256:
                output.append(self.dictionary[code])
                continue
            distance = code - 256
            length = reader.read_code() + 3
            if distance <= 0 or distance > len(output):
                raise ValueError(
                    f"条目 {entry.index} 回溯距离无效：{distance}，已输出 {len(output)} 字节"
                )
            if len(output) + length > entry.uncomp_size:
                raise ValueError(f"条目 {entry.index} 解压超出声明长度")
            for _ in range(length):
                output.append(output[-distance])
        return bytes(output)


# --------------------------------------------------------------------------
# 1. 2.PAT 图案块（语义未定）
# --------------------------------------------------------------------------
def glyph_image(glyph: bytes) -> Image.Image:
    pixels = [
        255 if glyph[y * 2 + x // 8] & (0x80 >> (x % 8)) else 0
        for y in range(16)
        for x in range(16)
    ]
    image = Image.new("L", (16, 16))
    image.putdata(pixels)
    return image


def export_pat2(source: Path, output: Path) -> dict:
    data = (source / "2.PAT").read_bytes()
    count = len(data) // 32
    out = output / "pat2"
    out.mkdir(parents=True, exist_ok=True)

    columns, rows_per_sheet = 100, 8
    sheet_size = columns * rows_per_sheet
    sheets = []
    for sheet_index in range((count + sheet_size - 1) // sheet_size):
        sheet = Image.new("L", (columns * 16, rows_per_sheet * 16))
        for j in range(sheet_size):
            index = sheet_index * sheet_size + j
            if index >= count:
                break
            sheet.paste(glyph_image(data[index * 32 : (index + 1) * 32]),
                        ((j % columns) * 16, (j // columns) * 16))
        rel = f"sheets/sheet-{sheet_index:02d}.png"
        safe_png(out / rel, sheet)
        sheets.append(rel)

    # 完整图集：100 列 x 68 行 = 1600 x 1088
    atlas_cols = 100
    atlas_rows = (count + atlas_cols - 1) // atlas_cols
    atlas = Image.new("L", (atlas_cols * 16, atlas_rows * 16))
    for index in range(count):
        atlas.paste(glyph_image(data[index * 32 : (index + 1) * 32]),
                    ((index % atlas_cols) * 16, (index // atlas_cols) * 16))
    safe_png(out / "atlas.png", atlas)

    info = {
        "source": "2.PAT",
        "source_size": len(data),
        "source_sha256": sha256(data),
        "block_count": count,
        "block_bytes": 32,
        "slice_geometry": "16 行 x 2 字节，MSB-first，16x16 单色位图切片",
        "width": 16,
        "height": 16,
        "semantics": "未确定：与 1.PAT 的 5401 个汉字点阵零精确匹配；6800 块互不相同、无空白块；"
                     "前段为对称符号/框线图案，零字节密度比字模更均匀",
        "confidence": "切片几何可信；内容不是 1.PAT 那套汉字字模，不要当作字库使用",
        "atlas": "atlas.png",
        "atlas_size": [atlas_cols * 16, atlas_rows * 16],
        "sheets": sheets,
    }
    write_json(out / "manifest.json", info)
    return info


# --------------------------------------------------------------------------
# 2. LZW 归档
# --------------------------------------------------------------------------
def portmap_png(raw: bytes) -> Image.Image:
    """PORTMAP 实测为 96x96 8bpp 线性索引；调色板未定位，用灰度表达结构。"""
    image = Image.new("L", (96, 96))
    image.putdata(list(raw[: 96 * 96]))
    return image


def export_lzw(source: Path, output: Path) -> dict:
    base = output / "lzw"
    base.mkdir(parents=True, exist_ok=True)
    summary = []
    for path in sorted(source.glob("*.LZW")):
        archive = KoeiArchive(path)
        target = base / path.stem
        (target / "bin").mkdir(parents=True, exist_ok=True)
        entries = []
        preview_images = []
        for entry in archive.entries:
            raw = archive.decompress(entry)
            rel = f"bin/{entry.index:03d}.bin"
            safe_bytes(target / rel, raw)
            record = {
                "index": entry.index,
                "offset": entry.offset,
                "compressed_size": entry.comp_size,
                "uncompressed_size": entry.uncomp_size,
                "raw": rel,
                "sha256": sha256(raw),
            }
            if len(raw) != entry.uncomp_size:
                record["warning"] = "解压长度与 FAT 声明不符"
            entries.append(record)

            if path.stem == "PORTMAP" and len(raw) == 9216:
                image = portmap_png(raw)
                prel = f"preview/{entry.index:03d}.png"
                safe_png(target / prel, image)
                record["preview"] = prel
                preview_images.append(image)

        contact_rel = None
        if preview_images:
            cols = 10
            rows = (len(preview_images) + cols - 1) // cols
            contact = Image.new("L", (cols * 96, rows * 96))
            for i, image in enumerate(preview_images):
                contact.paste(image, ((i % cols) * 96, (i // cols) * 96))
            contact_rel = "preview/contact-sheet.png"
            safe_png(target / contact_rel, contact)

        info = {
            "source": path.name,
            "source_size": len(archive.data),
            "source_sha256": sha256(archive.data),
            "format": archive.kind,
            "fat_end": archive.fat_end,
            "trailing_bytes_outside_fat": archive.tail_size,
            "entry_count": len(entries),
            "already_had_png_export": path.stem in ("KAO", "PORTCHIP"),
            "contact_sheet": contact_rel,
            "entries": entries,
        }
        write_json(target / "manifest.json", info)
        summary.append({
            "source": path.name,
            "format": archive.kind,
            "entries": len(entries),
            "trailing_bytes_outside_fat": archive.tail_size,
        })
        print(f"  {path.name:16s} {archive.kind}  {len(entries):4d} 条已解压")
    return {"archives": summary}


# --------------------------------------------------------------------------
# 3. DAT 偏移表容器
# --------------------------------------------------------------------------
def dat_offsets(data: bytes):
    """首 4 字节大端 = 偏移表字节长度；表内为连续大端 32 位偏移。"""
    if len(data) < 8:
        return None
    table_len = struct.unpack_from(">I", data, 0)[0]
    if not (4 <= table_len <= len(data) // 2 and table_len % 4 == 0):
        return None
    offsets = [struct.unpack_from(">I", data, i)[0] for i in range(0, table_len, 4)]
    if offsets[0] != table_len:
        return None
    if not all(offsets[i] <= offsets[i + 1] for i in range(len(offsets) - 1)):
        return None
    if offsets[-1] > len(data):
        return None
    return offsets


def export_dat(source: Path, output: Path) -> dict:
    base = output / "dat-containers"
    base.mkdir(parents=True, exist_ok=True)
    summary = []
    for path in sorted(list(source.glob("*.DAT")) + list(source.glob("*.PUT"))):
        data = path.read_bytes()
        offsets = dat_offsets(data)
        if offsets is None:
            continue
        target = base / path.stem
        (target / "blocks").mkdir(parents=True, exist_ok=True)
        blocks = []
        for i, start in enumerate(offsets):
            end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
            block = data[start:end]
            rel = f"blocks/{i:03d}.bin"
            safe_bytes(target / rel, block)
            width = height = None
            if len(block) >= 4:
                w, h = struct.unpack_from("<HH", block, 0)
                if 0 < w <= 1024 and 0 < h <= 1024:
                    width, height = w, h
            blocks.append({
                "index": i,
                "start": start,
                "end": end,
                "size": len(block),
                "raw": rel,
                "width_header_le16": width,
                "height_header_le16": height,
                "sha256": sha256(block),
            })
        info = {
            "source": path.name,
            "source_size": len(data),
            "source_sha256": sha256(data),
            "table_length": offsets[0],
            "block_count": len(blocks),
            "note": "块首 4 字节为小端 width/height；块体为未解码的图形编码，本工具只做无损拆分",
            "blocks": blocks,
        }
        write_json(target / "manifest.json", info)
        summary.append({"source": path.name, "blocks": len(blocks)})
        print(f"  {path.name:16s} {len(blocks):4d} 块已拆分")
    return {"containers": summary}


# --------------------------------------------------------------------------
def write_overview(output: Path, parts: dict) -> None:
    # 排除清单自身，避免「统计时它是否存在」导致数字漂移、以及字节数自指
    files = [
        p for p in output.rglob("*")
        if p.is_file() and p != output / "manifest.json"
    ]
    overview = {
        "source_directory": "sea2",
        "note": "全部为新建文件；未删除、未覆盖 hd 下任何已有资源",
        "total_files": len(files) + 1,
        "total_bytes": sum(p.stat().st_size for p in files),
        "count_note": "total_files 计入清单自身；total_bytes 为其余产物之和（不含清单自身）",
        "parts": parts,
    }
    write_json(output / "manifest.json", overview)


def write_readme(output: Path) -> None:
    text = """# 补充解包资源（2026-11-08）

生成工具：`tools/koei_assets_extra.py`
来源：`sea2/`（只读）
本目录为**全新目录**，生成时拒绝写入任何已存在的文件，原有 `hd/` 资源未删除、未覆盖。

## 目录

| 目录 | 内容 | 确定性 |
| --- | --- | --- |
| `pat2/` | `2.PAT` 的 6800 个 32 字节块，按 16x16 单色位图切片的图集 `atlas.png` + 分卷 `sheets/` | 切片几何可信；**内容语义未定，不是汉字字库** |
| `lzw/` | 全部 14 个 `.LZW` 的每个条目解压后的 `.bin` | **无损**：解压长度逐条与 FAT 声明核对 |
| `lzw/PORTMAP/preview/` | 101 张 96x96 8bpp 图块的灰度预览 + 总览图 | 尺寸与线性布局已验证；**灰度不等于游戏颜色** |
| `dat-containers/` | GRAPH / GRAPH2 / ENDGRP / EVENT0..6 按偏移表拆出的子块 `.bin` | **无损拆分**；块体图形编码未解 |

每个子目录都有 `manifest.json`（源文件 sha256、条目偏移/长度、解压后 sha256）。

## 本轮新确认的格式事实

1. **LS10 与 LS11 共用同一个 LZ 内核**：同样的 16 字节头、256 字节置换字典、
   变长码 + 回溯距离/长度的解压方式。差别只在 FAT 结束标记：
   - LS11：`272` 起每 12 字节一条 `(comp_size, uncomp_size, offset)` 大端，末尾一条 12 字节全零；
   - LS10：同样每 12 字节一条，但末尾只有 **4 字节零**。
     LS10 条目数由第一条数据 offset 反推：`n = (first_offset - 276) / 12`
     （CHAR=7、IAE1/IAP1..6=35、OPGRAPH=14，9 个文件全部通过连续性与越界校验）。
   `OPGRAPH.LZW` 末尾还有约 29 KB 未列入 FAT 的数据，manifest 里记为 `trailing_bytes_outside_fat`。
2. **`2.PAT`**：217600 字节 = 6800 x 32 字节，整除、无文件头、无扇区填充。
   按「16 行 x 2 字节、MSB-first」解读能得到结构完整的 16x16 图案块，
   但**它不是 1.PAT 那套汉字点阵**：与 1.PAT 汉字区 5401 个位图做精确匹配，
   命中 0 条（试过块内偏移 0/2/4）；6800 块两两互不相同、无全零块，
   零字节比例均值 0.068 但最大仅 0.406（1.PAT 汉字最大 0.929），密度比字模均匀得多。
   此前计划文档把 `2.PAT` 推测为「16x16 字形 / 半角数字」，本轮**证伪了「与 1.PAT 同源字库」这层假设**，
   内容更可能是图形 pattern / 符号 / 图标块，具体编码与语义仍待定。
3. **PORTMAP.LZW**：101 条，每条恰好 9216 = 96x96 字节，8bpp 线性索引；
   按 96x96 渲染能看到清晰的建筑/窗格结构。真实 256 色调色板尚未定位，
   因此预览是灰度（数值直接当亮度），只用于看结构，不是游戏原始配色。
4. **DAT 偏移表容器**：首 4 字节大端 = 偏移表长度（等于第一块的起始偏移），
   表内为连续大端偏移；GRAPH.DAT 65 块、ENDGRP.DAT 59 块、EVENT0..6 各 1..6 块、
   GRAPH2.DAT 2 块。多数块首 4 字节是小端 `width,height`（如 640x400、224x160），
   块体为未解码的 RLE/位平面图形。

## 尚未解决（不要当成已完成）

- DAT 子块、WORLDMAP.LZW（16 位行偏移表 + RLE）、DATA1.LZW、OPGRAPH/IAP/IAE/CHAR
  的**内部图形编码**未解码，本目录只提供无损解压后的原始字节。
- `2.PAT` 6800 个块的语义未定；不要把它当汉字字库用，也不要用本目录的图集去补
  `1.PAT` 的半角/数字映射。要定性需另找运行时读取该文件的代码路径。
- PORTMAP 的游戏调色板未定位；需要时可从 MAIN.EXE 的 VGA DAC 表或运行时截帧比对。
- 本轮没有改动 `tools/koei_assets.py`、`assets/original`、`assets/hd`，
  也没有触碰 `sea2/` 里的任何文件。

## 复现

```bash
"C:/Users/SLY/AppData/Local/Programs/Python/Python310/python.exe" tools/koei_assets_extra.py \
  --output "D:/OldGames/大航海时代2/hd/extracted-<新目录名>"
```

`--output` 指向已存在的目录时工具会直接拒绝运行，防止覆盖。
"""
    (output / "README.md").write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="大航海时代2 补充美术资源解包")
    parser.add_argument("--source", type=Path, default=ROOT / "sea2")
    parser.add_argument("--output", type=Path, required=True,
                        help="必须是尚不存在的全新目录")
    args = parser.parse_args()

    output = args.output.resolve()
    if output.exists():
        raise SystemExit(f"输出目录已存在，为避免覆盖已有资源，拒绝运行：{output}")
    if not (args.source / "2.PAT").is_file():
        raise SystemExit(f"源目录缺少 2.PAT：{args.source}")

    print(f"输出：{output}")
    output.mkdir(parents=True)

    print("[1/3] 2.PAT 图案块")
    parts = {"pat2": export_pat2(args.source, output)}
    print("[2/3] LZW 归档")
    parts["lzw"] = export_lzw(args.source, output)
    print("[3/3] DAT 偏移表容器")
    parts["dat"] = export_dat(args.source, output)

    write_readme(output)
    write_overview(output, parts)
    print("完成")


if __name__ == "__main__":
    main()
