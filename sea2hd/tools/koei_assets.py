from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
KAO_PALETTE = [
    (0x00, 0x00, 0x00),
    (0x00, 0xA0, 0x60),
    (0xD0, 0x40, 0x00),
    (0xF0, 0xA0, 0x60),
    (0x00, 0x40, 0xD0),
    (0x00, 0xA0, 0xF0),
    (0xD0, 0x60, 0xA0),
    (0xF0, 0xE0, 0xD0),
]
PORTCHIP_PALETTES = {
    "dawn": [
        (0x00, 0x00, 0x00), (0x82, 0x92, 0xA2), (0x00, 0xA2, 0x61), (0x00, 0x61, 0xA2),
        (0xD3, 0x41, 0x00), (0xA2, 0x71, 0x00), (0xF3, 0xA2, 0x61), (0x00, 0x92, 0x82),
        (0x00, 0x41, 0xD3), (0x00, 0x41, 0xA2), (0x00, 0xA2, 0xF3), (0x00, 0x30, 0x41),
        (0xD3, 0x61, 0xA2), (0x92, 0x82, 0x20), (0xF3, 0xE3, 0xD3), (0xD3, 0xE3, 0xF3),
    ],
    "day": [
        (0x00, 0x00, 0x00), (0x71, 0x71, 0x92), (0x00, 0xA2, 0x61), (0x00, 0x82, 0xF3),
        (0xD3, 0x41, 0x00), (0xA2, 0x61, 0x00), (0xF3, 0xA2, 0x61), (0x00, 0xB2, 0x61),
        (0x00, 0x41, 0xD3), (0x00, 0x41, 0xC3), (0x00, 0xA2, 0xF3), (0x00, 0x71, 0x61),
        (0xD3, 0x61, 0xA2), (0xE3, 0xB2, 0x51), (0xF3, 0xE3, 0xD3), (0xF3, 0xE3, 0xD3),
    ],
    "dusk": [
        (0x00, 0x00, 0x00), (0x82, 0x61, 0x30), (0x00, 0xA2, 0x61), (0x61, 0x71, 0x92),
        (0xD3, 0x41, 0x00), (0x92, 0x30, 0x00), (0xF3, 0xA2, 0x61), (0x92, 0x82, 0x61),
        (0x00, 0x41, 0xD3), (0x41, 0x51, 0x92), (0x00, 0xA2, 0xF3), (0x00, 0x41, 0x41),
        (0xD3, 0x61, 0xA2), (0xC3, 0x92, 0x71), (0xF3, 0xD3, 0xA2), (0xF3, 0xE3, 0xD3),
    ],
    "night": [
        (0x00, 0x00, 0x00), (0x41, 0x41, 0x41), (0x00, 0xA2, 0x61), (0x00, 0x51, 0x61),
        (0xD3, 0x41, 0x00), (0x61, 0x41, 0x00), (0xF3, 0xA2, 0x61), (0x00, 0x41, 0x41),
        (0x00, 0x41, 0xD3), (0x00, 0x41, 0x71), (0x00, 0xA2, 0xF3), (0x00, 0x30, 0x41),
        (0xD3, 0x61, 0xA2), (0x51, 0x51, 0x30), (0xF3, 0xE3, 0xD3), (0x61, 0x61, 0x71),
    ],
}


@dataclass(frozen=True)
class LS11Entry:
    index: int
    comp_size: int
    uncomp_size: int
    offset: int


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.bit_position = 0

    def read_bit(self) -> int:
        byte_position = self.bit_position // 8
        if byte_position >= len(self.data):
            raise ValueError("LS11 位流意外结束")
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


class BitWriter:
    def __init__(self):
        self.data = bytearray()
        self.current_byte = 0
        self.bit_count = 0

    def write_bit(self, bit: int) -> None:
        self.current_byte = (self.current_byte << 1) | bit
        self.bit_count += 1
        if self.bit_count == 8:
            self.data.append(self.current_byte)
            self.current_byte = 0
            self.bit_count = 0

    def write_code(self, code: int) -> None:
        if code < 0:
            raise ValueError("LS11 编码码值不能为负数")
        count = (code + 2).bit_length() - 1
        for _ in range(count - 1):
            self.write_bit(1)
        self.write_bit(0)
        value = code - ((1 << count) - 2)
        for shift in range(count - 1, -1, -1):
            self.write_bit((value >> shift) & 1)

    def finish(self) -> bytes:
        if self.bit_count:
            self.data.append(self.current_byte << (8 - self.bit_count))
            self.current_byte = 0
            self.bit_count = 0
        return bytes(self.data)


def code_bit_size(code: int) -> int:
    return 2 * ((code + 2).bit_length() - 1)


def compress_ls11(raw: bytes, dictionary: bytes) -> bytes:
    if len(dictionary) != 256 or len(set(dictionary)) != 256:
        raise ValueError("LS11 字典必须包含 256 个不同字节")
    literal_codes = {byte: index for index, byte in enumerate(dictionary)}
    positions: dict[bytes, list[int]] = {}
    writer = BitWriter()
    position = 0
    window = 4096
    max_candidates = 48

    while position < len(raw):
        best_length = 0
        best_distance = 0
        if position + 3 <= len(raw):
            key = raw[position : position + 3]
            candidates = positions.get(key, [])
            checked = 0
            for previous in reversed(candidates):
                distance = position - previous
                if distance > window:
                    break
                checked += 1
                if checked > max_candidates:
                    break
                length = 3
                while (
                    position + length < len(raw)
                    and raw[previous + length] == raw[position + length]
                ):
                    length += 1
                if length > best_length:
                    best_length = length
                    best_distance = distance

        if best_length >= 3:
            match_cost = code_bit_size(best_distance + 256) + code_bit_size(
                best_length - 3
            )
            literal_cost = sum(
                code_bit_size(literal_codes[byte])
                for byte in raw[position : position + best_length]
            )
        else:
            match_cost = literal_cost = 0

        consumed = best_length if best_length >= 3 and match_cost < literal_cost else 1
        if consumed > 1:
            writer.write_code(best_distance + 256)
            writer.write_code(best_length - 3)
        else:
            writer.write_code(literal_codes[raw[position]])

        for added in range(position, position + consumed):
            if added + 3 <= len(raw):
                positions.setdefault(raw[added : added + 3], []).append(added)
        position += consumed

    return writer.finish()


class LS11Archive:
    def __init__(self, source: Path):
        self.source = source
        self.data = source.read_bytes()
        if len(self.data) < 284 or self.data[:4] != b"LS11":
            raise ValueError(f"{source.name} 不是有效的 LS11 文件")
        self.header = self.data[:16]
        self.dictionary = self.data[16:272]
        self.entries: list[LS11Entry] = []
        position = 272
        while position + 12 <= len(self.data):
            comp_size, uncomp_size, offset = struct.unpack_from(
                ">III", self.data, position
            )
            position += 12
            if comp_size == 0:
                self.fat_end = position
                break
            if offset + comp_size > len(self.data):
                raise ValueError(f"条目 {len(self.entries)} 的数据超出文件末尾")
            self.entries.append(
                LS11Entry(len(self.entries), comp_size, uncomp_size, offset)
            )
        else:
            raise ValueError("未找到 LS11 FAT 结束标记")

    def decompress(self, entry: LS11Entry) -> bytes:
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
                    f"条目 {entry.index} 含无效回溯距离 {distance}，"
                    f"当前输出 {len(output)} 字节"
                )
            if len(output) + length > entry.uncomp_size:
                raise ValueError(f"条目 {entry.index} 解压后超过声明长度")
            for _ in range(length):
                output.append(output[-distance])
        return bytes(output)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode_planar(
    data: bytes, width: int, height: int, align_length: int, bpp: int
) -> list[int]:
    expected_size = width * height * bpp // 8
    if len(data) != expected_size:
        raise ValueError(
            f"位平面数据长度不符：实际 {len(data)}，预期 {expected_size}"
        )
    group_size = align_length * bpp
    if expected_size % group_size:
        raise ValueError("位平面数据未按对齐长度完整分组")

    pixels: list[int] = []
    for base in range(0, len(data), group_size):
        group = data[base : base + group_size]
        for bit_index in range(align_length * 8):
            byte_index = bit_index // 8
            mask = 0x80 >> (bit_index % 8)
            color = 0
            for plane in range(bpp):
                bit = (group[plane * align_length + byte_index] & mask) != 0
                color |= int(bit) << (bpp - plane - 1)
            pixels.append(color)
    return pixels


def encode_planar(
    pixels: list[int], width: int, height: int, align_length: int, bpp: int
) -> bytes:
    if len(pixels) != width * height:
        raise ValueError("像素数量与图片尺寸不符")
    if max(pixels, default=0) >= 1 << bpp or min(pixels, default=0) < 0:
        raise ValueError(f"图片含有超出 {bpp}bpp 范围的颜色索引")
    raw_size = width * height * bpp // 8
    group_size = align_length * bpp
    if raw_size % group_size or len(pixels) % (align_length * 8):
        raise ValueError("图片尺寸不能按指定位平面对齐方式编码")

    output = bytearray()
    position = 0
    while position < len(pixels):
        group = bytearray(group_size)
        for bit_index in range(align_length * 8):
            color = pixels[position + bit_index]
            byte_index = bit_index // 8
            mask = 0x80 >> (bit_index % 8)
            for plane in range(bpp):
                if color & (1 << (bpp - plane - 1)):
                    group[plane * align_length + byte_index] |= mask
        output.extend(group)
        position += align_length * 8
    return bytes(output)


def indexed_image(
    pixels: list[int], width: int, height: int, palette: list[tuple[int, int, int]]
) -> Image.Image:
    image = Image.new("P", (width, height))
    image.putdata(pixels)
    flat_palette = [component for color in palette for component in color]
    image.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    return image


def make_sheet(
    images: list[Image.Image], width: int, height: int, columns: int
) -> Image.Image:
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("P", (columns * width, rows * height))
    sheet.putpalette(images[0].getpalette())
    for index, image in enumerate(images):
        sheet.paste(image, ((index % columns) * width, (index // columns) * height))
    return sheet


def relative(path: Path, base: Path) -> str:
    return path.relative_to(base).as_posix()


def export_kao(archive: LS11Archive, output: Path, palette_path: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    bin_dir = output / "bin"
    bin_dir.mkdir(exist_ok=True)
    category_dirs = {
        category: output / category
        for category in ("portraits", "discoveries", "items", "other")
    }
    for directory in category_dirs.values():
        directory.mkdir(exist_ok=True)
    sheets_dir = output / "sheets"
    sheets_dir.mkdir(exist_ok=True)

    palette = KAO_PALETTE
    palette_path.parent.mkdir(parents=True, exist_ok=True)
    palette_path.write_bytes(bytes(c for rgb in palette for c in rgb))
    manifest_entries = []
    sheet_groups: dict[str, list[Image.Image]] = {
        "portraits": [],
        "discoveries": [],
        "items": [],
        "other": [],
    }

    for entry in archive.entries:
        raw = archive.decompress(entry)
        raw_path = bin_dir / f"{entry.index:03d}.bin"
        raw_path.write_bytes(raw)
        if entry.index < 128:
            category = "portraits"
        elif entry.index <= 212:
            category = "discoveries"
        elif entry.index <= 255:
            category = "items"
        else:
            category = "other"

        dimensions = None
        png_path = None
        used_colors: list[int] = []
        if len(raw) == 1920:
            dimensions = (64, 80)
        elif len(raw) == 864:
            dimensions = (48, 48)
        if dimensions:
            width, height = dimensions
            pixels = decode_planar(raw, width, height, align_length=1, bpp=3)
            image = indexed_image(pixels, width, height, palette)
            png_path = category_dirs[category] / f"{entry.index:03d}.png"
            image.save(png_path)
            sheet_groups[category].append(image)
            used_colors = sorted(set(pixels))

        manifest_entries.append(
            {
                "index": entry.index,
                "category": category,
                "offset": entry.offset,
                "compressed_size": entry.comp_size,
                "decompressed_size": entry.uncomp_size,
                "raw": relative(raw_path, output),
                "sha256": sha256(raw),
                "width": dimensions[0] if dimensions else None,
                "height": dimensions[1] if dimensions else None,
                "png": relative(png_path, output) if png_path else None,
                "used_colors": used_colors,
            }
        )

    sheets = {}
    for category, images in sheet_groups.items():
        if not images:
            continue
        width, height = (64, 80) if category == "portraits" else (48, 48)
        columns = 8 if category == "portraits" else 16
        sheet = make_sheet(images, width, height, columns)
        sheet_path = sheets_dir / f"{category}.png"
        sheet.save(sheet_path)
        sheets[category] = relative(sheet_path, output)

    return {
        "format": "LS11",
        "source_file": archive.source.name,
        "source_size": len(archive.data),
        "source_sha256": sha256(archive.data),
        "encoding": {"bpp": 3, "align_length": 1, "plane_order": "MSB-first"},
        "palette": f"../palettes/{palette_path.name}",
        "entry_count": len(archive.entries),
        "entries": manifest_entries,
        "sheets": sheets,
    }


def export_portchip(archive: LS11Archive, output: Path, palette_path: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    bin_dir = output / "bin"
    tiles_dir = output / "tiles"
    sheets_dir = output / "sheets"
    for directory in (bin_dir, tiles_dir, sheets_dir):
        directory.mkdir(exist_ok=True)

    palette = PORTCHIP_PALETTES["day"]
    palette_path.parent.mkdir(parents=True, exist_ok=True)
    palette_path.write_bytes(bytes(c for rgb in palette for c in rgb))
    for name, variant in PORTCHIP_PALETTES.items():
        variant_path = palette_path.with_name(f"PORTCHIP_{name.upper()}.PAL")
        variant_path.write_bytes(bytes(c for rgb in variant for c in rgb))
    manifest_entries = []
    all_used_colors: set[int] = set()

    for entry in archive.entries:
        raw = archive.decompress(entry)
        raw_path = bin_dir / f"{entry.index:03d}.bin"
        raw_path.write_bytes(raw)
        if not raw or len(raw) % 128:
            manifest_entries.append(
                {
                    "index": entry.index,
                    "offset": entry.offset,
                    "compressed_size": entry.comp_size,
                    "decompressed_size": entry.uncomp_size,
                    "raw": relative(raw_path, output),
                    "sha256": sha256(raw),
                    "tile_count": 0,
                    "sheet": None,
                    "tiles": [],
                    "note": "非芯片数据，原样保留",
                }
            )
            continue
        tile_count = len(raw) // 128
        pixels = decode_planar(
            raw, width=16, height=tile_count * 16, align_length=32, bpp=4
        )
        tiles = []
        tile_images = []
        for tile_index in range(tile_count):
            start = tile_index * 256
            tile_pixels = pixels[start : start + 256]
            image = indexed_image(tile_pixels, 16, 16, palette)
            tile_path = tiles_dir / f"{entry.index:03d}_{tile_index:03d}.png"
            image.save(tile_path)
            tile_images.append(image)
            colors = sorted(set(tile_pixels))
            all_used_colors.update(colors)
            tiles.append(
                {
                    "index": tile_index,
                    "png": relative(tile_path, output),
                    "used_colors": colors,
                }
            )
        sheet = make_sheet(tile_images, 16, 16, columns=min(40, tile_count))
        sheet_path = sheets_dir / f"entry_{entry.index:03d}.png"
        sheet.save(sheet_path)
        manifest_entries.append(
            {
                "index": entry.index,
                "offset": entry.offset,
                "compressed_size": entry.comp_size,
                "decompressed_size": entry.uncomp_size,
                "raw": relative(raw_path, output),
                "sha256": sha256(raw),
                "tile_width": 16,
                "tile_height": 16,
                "tile_count": tile_count,
                "sheet": relative(sheet_path, output),
                "tiles": tiles,
            }
        )

    return {
        "format": "LS11",
        "source_file": archive.source.name,
        "source_size": len(archive.data),
        "source_sha256": sha256(archive.data),
        "encoding": {"bpp": 4, "align_length": 32, "plane_order": "MSB-first"},
        "palette": f"../palettes/{palette_path.name}",
        "palette_status": "完整 16 色；默认使用白天配色，另附黎明、黄昏、夜晚调色板",
        "palette_source": "https://emla.tistory.com/252",
        "palettes": {
            name: f"../palettes/PORTCHIP_{name.upper()}.PAL"
            for name in PORTCHIP_PALETTES
        },
        "used_colors": sorted(all_used_colors),
        "entry_count": len(archive.entries),
        "entries": manifest_entries,
    }


def extract(source: Path, output: Path) -> None:
    exporters = {"KAO.LZW": export_kao, "PORTCHIP.LZW": export_portchip}
    for filename, exporter in exporters.items():
        source_path = source / filename
        archive = LS11Archive(source_path)
        target = output / source_path.stem
        manifest = exporter(archive, target, output / "palettes" / f"{source_path.stem}.PAL")
        manifest_path = target / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(
            f"{filename}: {len(archive.entries)} 个条目 -> "
            f"{target} (manifest: {manifest_path.name})"
        )


def image_to_pixels(
    image_path: Path, palette: list[tuple[int, int, int]]
) -> tuple[tuple[int, int], list[int]]:
    with Image.open(image_path) as source:
        image = source.copy()
    if image.mode == "P":
        return image.size, list(image.get_flattened_data())

    rgb_image = image.convert("RGB")
    cache: dict[tuple[int, int, int], int] = {}
    pixels = []
    for color in rgb_image.get_flattened_data():
        if color not in cache:
            cache[color] = min(
                range(len(palette)),
                key=lambda index: sum(
                    (color[channel] - palette[index][channel]) ** 2
                    for channel in range(3)
                ),
            )
        pixels.append(cache[color])
    return image.size, pixels


def repack(
    asset: str,
    images: Path | None,
    output: Path,
    original_root: Path,
    game_root: Path,
) -> None:
    source_path = game_root / f"{asset}.LZW"
    asset_root = original_root / asset
    manifest = json.loads((asset_root / "manifest.json").read_text(encoding="utf-8"))
    palette_path = original_root / "palettes" / f"{asset}.PAL"
    palette_bytes = palette_path.read_bytes()
    palette = [tuple(palette_bytes[i : i + 3]) for i in range(0, len(palette_bytes), 3)]
    archive = LS11Archive(source_path)
    if len(archive.entries) != len(manifest["entries"]):
        raise ValueError("清单条目数与原游戏文件不一致")

    data_start = min(entry.offset for entry in archive.entries)
    fat_end = 272 + 12 * len(archive.entries)
    fat_suffix = archive.data[fat_end:data_start]
    tail_start = max(entry.offset + entry.comp_size for entry in archive.entries)
    tail = archive.data[tail_start:]
    next_offset = data_start
    fat_entries = []
    blocks = []
    expected_raw_entries = []
    changed_entries = 0

    for entry, metadata in zip(archive.entries, manifest["entries"]):
        original_raw = archive.decompress(entry)
        raw = original_raw
        if images is not None and asset == "KAO" and metadata["png"]:
            (width, height), pixels = image_to_pixels(images / metadata["png"], palette)
            raw = encode_planar(pixels, width, height, align_length=1, bpp=3)
        elif images is not None and asset == "PORTCHIP" and metadata["tile_count"]:
            # 芯片按整张位平面存储；放大后每个芯片独立编码再拼接。
            tile_blocks = []
            for tile in metadata["tiles"]:
                (width, height), pixels = image_to_pixels(images / tile["png"], palette)
                align_length = width * height // 8
                tile_blocks.append(
                    encode_planar(pixels, width, height, align_length, bpp=4)
                )
            raw = b"".join(tile_blocks)

        if raw != original_raw:
            changed_entries += 1
            compressed = compress_ls11(raw, archive.dictionary)
            if len(compressed) >= len(raw):
                compressed = raw
        else:
            compressed = archive.data[entry.offset : entry.offset + entry.comp_size]
        if not compressed:
            raise ValueError(f"条目 {entry.index} 为空，无法作为有效 LS11 FAT 项回封")

        fat_entries.append(struct.pack(">III", len(compressed), len(raw), next_offset))
        blocks.append(compressed)
        expected_raw_entries.append(raw)
        next_offset += len(compressed)

    result = (
        archive.data[:272]
        + b"".join(fat_entries)
        + fat_suffix
        + b"".join(blocks)
        + tail
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)

    verified = LS11Archive(output)
    for entry, expected in zip(verified.entries, expected_raw_entries):
        actual = verified.decompress(entry)
        if actual != expected:
            raise ValueError(f"回封校验失败：条目 {entry.index} 解压结果不一致")

    binary_identical = result == archive.data
    report = {
        "asset": asset,
        "original_file": source_path.name,
        "original_sha256": sha256(archive.data),
        "output_file": output.name,
        "output_size": len(result),
        "output_sha256": sha256(result),
        "images": str(images) if images else None,
        "changed_entries": changed_entries,
        "entry_count": len(expected_raw_entries),
        "decompressed_verification": "passed",
        "binary_identical_to_original": binary_identical,
        "tail_size": len(tail),
    }
    report_path = output.with_suffix(output.suffix + ".json")
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        f"{asset}: 回封 {len(expected_raw_entries)} 项，修改 {changed_entries} 项，"
        f"逐条解压校验通过；与原件二进制一致={binary_identical}"
    )
    print(f"输出：{output}")


def upscale(
    assets: list[str], source_root: Path, output_root: Path, scale: int, method: str
) -> None:
    if method == "hqx" and scale not in (2, 3, 4):
        raise ValueError("HQX 仅支持 2、3、4 倍")
    if scale < 2:
        raise ValueError("放大倍数必须至少为 2")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        bundled_ffmpeg = Path(r"C:\Program Files\ffmpeg\bin\ffmpeg.exe")
        if bundled_ffmpeg.exists():
            ffmpeg = str(bundled_ffmpeg)

    for asset in assets:
        source = source_root / asset
        target = output_root / asset
        png_paths = sorted(source.rglob("*.png"))
        for source_path in png_paths:
            target_path = target / source_path.relative_to(source)
            target_path.parent.mkdir(parents=True, exist_ok=True)
            if method == "nearest":
                with Image.open(source_path) as image:
                    resized = image.resize(
                        (image.width * scale, image.height * scale),
                        Image.Resampling.NEAREST,
                    )
                    resized.save(target_path)
            else:
                if not ffmpeg:
                    raise RuntimeError("未找到 ffmpeg，无法执行 HQX 放大")
                subprocess.run(
                    [
                        ffmpeg,
                        "-hide_banner",
                        "-loglevel",
                        "error",
                        "-y",
                        "-i",
                        str(source_path),
                        "-vf",
                        f"hqx={scale}",
                        "-frames:v",
                        "1",
                        str(target_path),
                    ],
                    check=True,
                )
        shutil.copy2(source / "manifest.json", target / "manifest.json")
        manifest_path = target / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["upscale"] = {
            "method": method,
            "scale": scale,
            "entry_dimensions": "清单中保留的是原始尺寸；PNG 已按 scale 放大",
        }
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        palette_target_dir = output_root / "palettes"
        palette_target_dir.mkdir(parents=True, exist_ok=True)
        for palette_source in (source_root / "palettes").glob(f"{asset}*.PAL"):
            shutil.copy2(palette_source, palette_target_dir / palette_source.name)
        print(f"{asset}: {method} {scale}x，已处理 {len(png_paths)} 张 PNG -> {target}")


def _unique_runtime_items(items: list[dict], label: str) -> dict[int, dict]:
    unique = {}
    for item in items:
        index = item["index"]
        if index in unique:
            raise ValueError(f"{label} {index} 重复")
        unique[index] = item
    return unique


def _matched_runtime_entries(
    original: dict, hd: dict, asset: str
) -> list[tuple[dict, dict]]:
    for label, manifest in (("原始", original), ("HD", hd)):
        if manifest.get("entry_count") != len(manifest["entries"]):
            raise ValueError(f"{asset} {label}清单的 entry_count 与实际条目数不一致")
    original_entries = _unique_runtime_items(
        original["entries"], f"{asset} 原始清单条目"
    )
    hd_entries = _unique_runtime_items(hd["entries"], f"{asset} HD 清单条目")
    if original_entries.keys() != hd_entries.keys():
        raise ValueError(f"{asset} 原始清单与 HD 清单的条目不匹配")
    return [(entry, hd_entries[entry["index"]]) for entry in original["entries"]]


def _runtime_texture(
    hd_root: Path,
    asset: str,
    png_name: str,
    original_width: int,
    original_height: int,
    scale: int,
    seen_paths: set[str],
) -> dict:
    if not png_name:
        raise ValueError(f"{asset} 图像条目缺少独立 PNG 路径")
    png_relative = Path(png_name)
    if png_relative.is_absolute() or ".." in png_relative.parts:
        raise ValueError(f"{asset} PNG 路径必须位于对应 HD 资源目录内：{png_name}")
    if any(part.lower() == "sheets" for part in png_relative.parts):
        raise ValueError(f"{asset} 运行时清单只能索引独立素材，不能索引 sheets")

    png_path = hd_root / asset / png_relative
    output_path = relative(png_path, hd_root)
    if output_path in seen_paths:
        raise ValueError(f"运行时 PNG 路径重复：{output_path}")
    if not png_path.is_file():
        raise ValueError(f"找不到 HD PNG：{png_path}")

    with Image.open(png_path) as image:
        hd_size = image.size
        mode = image.mode
    expected_size = (original_width * scale, original_height * scale)
    if hd_size != expected_size:
        raise ValueError(
            f"{output_path} HD 尺寸为 {hd_size[0]}x{hd_size[1]}，"
            f"预期 {expected_size[0]}x{expected_size[1]}"
        )
    if mode != "RGBA":
        raise ValueError(f"{output_path} 图片模式为 {mode}，必须为 RGBA")

    seen_paths.add(output_path)
    return {
        "hd_width": hd_size[0],
        "hd_height": hd_size[1],
        "png": output_path,
        "mode": mode,
        "sha256": sha256(png_path.read_bytes()),
    }


def _kao_runtime_entries(
    original: dict, hd: dict, hd_root: Path, scale: int, seen_paths: set[str]
) -> list[dict]:
    runtime_entries = []
    excluded = {
        "index", "offset", "compressed_size", "decompressed_size", "raw",
        "sha256", "width", "height", "png", "used_colors",
    }
    for original_entry, hd_entry in _matched_runtime_entries(original, hd, "KAO"):
        index = original_entry["index"]
        for field in ("category", "width", "height", "png"):
            if original_entry.get(field) != hd_entry.get(field):
                raise ValueError(f"KAO 条目 {index} 的 {field} 与 HD 清单不一致")

        entry = {"entry": index}
        entry.update(
            {key: value for key, value in hd_entry.items() if key not in excluded}
        )
        width = original_entry["width"]
        height = original_entry["height"]
        png_name = original_entry["png"]
        entry.update({"original_width": width, "original_height": height})
        if png_name is None:
            if width is not None or height is not None:
                raise ValueError(f"KAO 非图像条目 {index} 不应包含原始图片尺寸")
            entry.update(
                {"hd_width": None, "hd_height": None, "png": None, "mode": None,
                 "sha256": None}
            )
        else:
            if width is None or height is None:
                raise ValueError(f"KAO 图像条目 {index} 缺少原始尺寸")
            entry.update(
                _runtime_texture(
                    hd_root, "KAO", png_name, width, height, scale, seen_paths
                )
            )
        runtime_entries.append(entry)
    return runtime_entries


def _portchip_runtime_entries(
    original: dict, hd: dict, hd_root: Path, scale: int, seen_paths: set[str]
) -> list[dict]:
    runtime_entries = []
    entry_excluded = {
        "index", "offset", "compressed_size", "decompressed_size", "raw",
        "sha256", "tile_width", "tile_height", "tile_count", "sheet", "tiles",
    }
    tile_excluded = {"index", "png", "used_colors"}
    for original_entry, hd_entry in _matched_runtime_entries(
        original, hd, "PORTCHIP"
    ):
        index = original_entry["index"]
        for field in ("tile_width", "tile_height", "tile_count"):
            if original_entry.get(field) != hd_entry.get(field):
                raise ValueError(f"PORTCHIP 条目 {index} 的 {field} 与 HD 清单不一致")

        entry = {"entry": index}
        entry.update(
            {
                key: value
                for key, value in hd_entry.items()
                if key not in entry_excluded
            }
        )
        tile_count = original_entry["tile_count"]
        entry["tile_count"] = tile_count
        original_tiles = _unique_runtime_items(
            original_entry["tiles"], f"PORTCHIP 条目 {index} 的原始芯片"
        )
        hd_tiles = _unique_runtime_items(
            hd_entry["tiles"], f"PORTCHIP 条目 {index} 的 HD 芯片"
        )
        if original_tiles.keys() != hd_tiles.keys():
            raise ValueError(f"PORTCHIP 条目 {index} 的原始与 HD 芯片不匹配")
        if len(original_tiles) != tile_count:
            raise ValueError(f"PORTCHIP 条目 {index} 的 tile_count 与芯片数不一致")
        if not tile_count:
            entry["tiles"] = []
            runtime_entries.append(entry)
            continue

        tile_width = original_entry["tile_width"]
        tile_height = original_entry["tile_height"]
        if (tile_width, tile_height) != (16, 16):
            raise ValueError(f"PORTCHIP 条目 {index} 的原始芯片尺寸必须为 16x16")
        entry.update({"tile_width": tile_width, "tile_height": tile_height})
        tiles = []
        for original_tile in original_entry["tiles"]:
            tile_index = original_tile["index"]
            hd_tile = hd_tiles[tile_index]
            if original_tile.get("png") != hd_tile.get("png"):
                raise ValueError(
                    f"PORTCHIP 条目 {index} 芯片 {tile_index} 的 PNG 与 HD 清单不一致"
                )
            tile = {"tile": tile_index}
            tile.update(
                {
                    key: value
                    for key, value in hd_tile.items()
                    if key not in tile_excluded
                }
            )
            tile.update(
                {"original_width": tile_width, "original_height": tile_height}
            )
            tile.update(
                _runtime_texture(
                    hd_root,
                    "PORTCHIP",
                    original_tile["png"],
                    tile_width,
                    tile_height,
                    scale,
                    seen_paths,
                )
            )
            tiles.append(tile)
        entry["tiles"] = tiles
        runtime_entries.append(entry)
    return runtime_entries


def build_runtime_manifest(
    original_root: Path, hd_root: Path, game_root: Path
) -> dict:
    scale = 4
    main_path = game_root / "MAIN.EXE"
    if not main_path.is_file():
        raise ValueError(f"找不到 MAIN.EXE：{main_path}")

    assets = {}
    builders = {
        "KAO": _kao_runtime_entries,
        "PORTCHIP": _portchip_runtime_entries,
    }
    seen_paths: set[str] = set()
    for asset, builder in builders.items():
        original = json.loads(
            (original_root / asset / "manifest.json").read_text(encoding="utf-8")
        )
        hd = json.loads(
            (hd_root / asset / "manifest.json").read_text(encoding="utf-8")
        )
        source_file = f"{asset}.LZW"
        if (
            original.get("source_file") != source_file
            or hd.get("source_file") != source_file
        ):
            raise ValueError(f"{asset} 清单的 source_file 必须为 {source_file}")
        if original.get("source_sha256") != hd.get("source_sha256"):
            raise ValueError(f"{asset} 原始清单与 HD 清单的资源 SHA256 不一致")
        if hd.get("upscale", {}).get("scale") != scale:
            raise ValueError(f"{asset} HD 清单的缩放倍数必须为 {scale}")

        source_path = game_root / source_file
        if not source_path.is_file():
            raise ValueError(f"找不到原始资源：{source_path}")
        source_sha256 = sha256(source_path.read_bytes())
        if source_sha256 != original["source_sha256"]:
            raise ValueError(f"{source_file} 的 SHA256 与当前原始清单不一致")

        runtime_asset = {
            key: value
            for key, value in hd.items()
            if key not in ("entries", "sheets")
        }
        runtime_asset["entries"] = builder(
            original, hd, hd_root, scale, seen_paths
        )
        assets[asset] = runtime_asset

    return {
        "format_version": 1,
        "scale": scale,
        "main_exe": {
            "file": main_path.name,
            "sha256": sha256(main_path.read_bytes()),
        },
        "assets": assets,
    }


def export_runtime_manifest(
    original_root: Path, hd_root: Path, game_root: Path, output: Path
) -> dict:
    manifest = build_runtime_manifest(original_root, hd_root, game_root)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    kao_count = sum(
        entry["png"] is not None for entry in manifest["assets"]["KAO"]["entries"]
    )
    portchip_count = sum(
        len(entry["tiles"])
        for entry in manifest["assets"]["PORTCHIP"]["entries"]
    )
    print(
        f"运行时清单：KAO {kao_count} 张，PORTCHIP {portchip_count} 张独立 RGBA PNG"
    )
    print(f"输出：{output}")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description="大航海时代2 KOEI 美术资源工具")
    subparsers = parser.add_subparsers(dest="command", required=True)
    extract_parser = subparsers.add_parser("extract", help="解包并导出原始图片")
    extract_parser.add_argument("--source", type=Path, default=ROOT / "sea2")
    extract_parser.add_argument("--output", type=Path, default=ROOT / "assets" / "original")

    upscale_parser = subparsers.add_parser("upscale", help="放大已导出的 PNG")
    upscale_parser.add_argument("--assets", nargs="+", choices=["KAO", "PORTCHIP"], default=["KAO", "PORTCHIP"])
    upscale_parser.add_argument("--source", type=Path, default=ROOT / "assets" / "original")
    upscale_parser.add_argument("--output", type=Path)
    upscale_parser.add_argument("--scale", type=int, default=4)
    upscale_parser.add_argument("--method", choices=["nearest", "hqx"], default="nearest")

    runtime_parser = subparsers.add_parser(
        "runtime-manifest", help="生成 DOSBox 运行时 RGBA 高清清单"
    )
    runtime_parser.add_argument(
        "--original", type=Path, default=ROOT / "assets" / "original"
    )
    runtime_parser.add_argument("--hd", type=Path, default=ROOT / "assets" / "hd")
    runtime_parser.add_argument("--game", type=Path, default=ROOT / "sea2")
    runtime_parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "assets" / "hd" / "runtime-manifest.json",
    )

    repack_parser = subparsers.add_parser("repack", help="将 PNG 重新编码并回封 LS11")
    repack_parser.add_argument("--asset", choices=["KAO", "PORTCHIP"], required=True)
    repack_parser.add_argument("--images", type=Path, help="与原始导出目录结构一致的 PNG 目录")
    repack_parser.add_argument("--output", type=Path, required=True)
    repack_parser.add_argument("--original", type=Path, default=ROOT / "assets" / "original")
    repack_parser.add_argument("--game", type=Path, default=ROOT / "sea2")

    args = parser.parse_args()
    if args.command == "extract":
        extract(args.source, args.output)
    elif args.command == "upscale":
        output = args.output or ROOT / "assets" / ("hd" if args.method == "hqx" else "hd-indexed")
        upscale(args.assets, args.source, output, args.scale, args.method)
    elif args.command == "runtime-manifest":
        export_runtime_manifest(args.original, args.hd, args.game, args.output)
    elif args.command == "repack":
        repack(args.asset, args.images, args.output, args.original, args.game)


if __name__ == "__main__":
    main()
