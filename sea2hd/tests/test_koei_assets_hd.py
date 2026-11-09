import copy
import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from tools.koei_assets import export_runtime_manifest, sha256


class RuntimeManifestTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.original = self.root / "assets" / "original"
        self.hd = self.root / "assets" / "hd"
        self.game = self.root / "sea2"
        self.game.mkdir(parents=True)
        (self.game / "MAIN.EXE").write_bytes(b"fixture MAIN.EXE")

        self.kao = {
            "format": "LS11",
            "source_file": "KAO.LZW",
            "source_size": 12,
            "encoding": {"bpp": 3},
            "entry_count": 2,
            "entries": [
                {
                    "index": 0,
                    "category": "portraits",
                    "offset": 100,
                    "compressed_size": 2,
                    "decompressed_size": 2,
                    "raw": "bin/000.bin",
                    "sha256": "original raw 0",
                    "width": 2,
                    "height": 3,
                    "png": "portraits/000.png",
                    "used_colors": [0],
                },
                {
                    "index": 1,
                    "category": "other",
                    "offset": 102,
                    "compressed_size": 2,
                    "decompressed_size": 2,
                    "raw": "bin/001.bin",
                    "sha256": "original raw 1",
                    "width": None,
                    "height": None,
                    "png": None,
                    "used_colors": [],
                    "note": "非图像数据，原样保留",
                },
            ],
            "sheets": {"portraits": "sheets/portraits.png"},
        }
        palettes = {
            name: f"../palettes/PORTCHIP_{name.upper()}.PAL"
            for name in ("dawn", "day", "dusk", "night")
        }
        self.portchip = {
            "format": "LS11",
            "source_file": "PORTCHIP.LZW",
            "source_size": 17,
            "encoding": {"bpp": 4},
            "palette_status": "四时段配色",
            "palette_source": "fixture",
            "palettes": palettes,
            "used_colors": [0, 1],
            "entry_count": 2,
            "entries": [
                {
                    "index": 0,
                    "culture": "fixture-culture",
                    "offset": 200,
                    "compressed_size": 2,
                    "decompressed_size": 256,
                    "raw": "bin/000.bin",
                    "sha256": "original tiles 0",
                    "tile_width": 16,
                    "tile_height": 16,
                    "tile_count": 2,
                    "sheet": "sheets/entry_000.png",
                    "tiles": [
                        {
                            "index": tile_index,
                            "png": f"tiles/000_{tile_index:03d}.png",
                            "used_colors": [tile_index],
                        }
                        for tile_index in range(2)
                    ],
                },
                {
                    "index": 1,
                    "offset": 202,
                    "compressed_size": 2,
                    "decompressed_size": 4,
                    "raw": "bin/001.bin",
                    "sha256": "original raw 1",
                    "tile_count": 0,
                    "sheet": None,
                    "tiles": [],
                    "note": "非芯片数据，原样保留",
                },
            ],
        }

        for asset, manifest in (("KAO", self.kao), ("PORTCHIP", self.portchip)):
            source = f"{asset} original".encode()
            (self.game / f"{asset}.LZW").write_bytes(source)
            manifest["source_size"] = len(source)
            manifest["source_sha256"] = sha256(source)
            self._write_manifest(self.original, asset, manifest)
            hd_manifest = copy.deepcopy(manifest)
            hd_manifest["upscale"] = {
                "method": "hqx",
                "scale": 4,
                "entry_dimensions": "清单中保留原始尺寸",
            }
            self._write_manifest(self.hd, asset, hd_manifest)

        self._save_png("KAO/portraits/000.png", (8, 12))
        self._save_png("KAO/sheets/portraits.png", (8, 12))
        self._save_png("PORTCHIP/tiles/000_000.png", (64, 64))
        self._save_png("PORTCHIP/tiles/000_001.png", (64, 64))
        self._save_png("PORTCHIP/sheets/entry_000.png", (128, 64))

    def _write_manifest(self, root, asset, manifest):
        target = root / asset / "manifest.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
        )

    def _save_png(self, relative_path, size, mode="RGBA"):
        path = self.hd / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        color = (10, 20, 30, 40) if mode == "RGBA" else (10, 20, 30)
        Image.new(mode, size, color).save(path)

    def _export(self):
        output = self.hd / "runtime-manifest.json"
        return export_runtime_manifest(self.original, self.hd, self.game, output), output

    def test_generates_complete_runtime_manifest(self):
        manifest, output = self._export()
        self.assertEqual(
            json.loads(output.read_text(encoding="utf-8")), manifest
        )
        self.assertEqual(manifest["format_version"], 1)
        self.assertEqual(manifest["scale"], 4)
        self.assertEqual(manifest["main_exe"]["file"], "MAIN.EXE")
        self.assertEqual(
            manifest["main_exe"]["sha256"],
            sha256((self.game / "MAIN.EXE").read_bytes()),
        )

        kao = manifest["assets"]["KAO"]
        self.assertEqual(kao["source_sha256"], sha256(b"KAO original"))
        self.assertEqual(len(kao["entries"]), 2)
        kao_image = kao["entries"][0]
        self.assertEqual(kao_image["entry"], 0)
        self.assertEqual(kao_image["category"], "portraits")
        self.assertEqual(
            (kao_image["original_width"], kao_image["original_height"]), (2, 3)
        )
        self.assertEqual((kao_image["hd_width"], kao_image["hd_height"]), (8, 12))
        self.assertEqual(kao_image["png"], "KAO/portraits/000.png")
        self.assertNotIn("\\", kao_image["png"])
        self.assertEqual(kao_image["mode"], "RGBA")
        self.assertEqual(
            kao_image["sha256"],
            sha256((self.hd / kao_image["png"]).read_bytes()),
        )
        kao_non_image = kao["entries"][1]
        self.assertEqual(kao_non_image["note"], "非图像数据，原样保留")
        self.assertIsNone(kao_non_image["png"])
        self.assertIsNone(kao_non_image["mode"])
        self.assertIsNone(kao_non_image["sha256"])

        portchip = manifest["assets"]["PORTCHIP"]
        self.assertEqual(portchip["source_sha256"], sha256(b"PORTCHIP original"))
        self.assertEqual(portchip["palettes"], self.portchip["palettes"])
        self.assertEqual(len(portchip["entries"]), 2)
        chip_entry = portchip["entries"][0]
        self.assertEqual(chip_entry["culture"], "fixture-culture")
        self.assertEqual(chip_entry["tile_count"], 2)
        self.assertEqual((chip_entry["tile_width"], chip_entry["tile_height"]), (16, 16))
        self.assertEqual(len(chip_entry["tiles"]), 2)
        paths = [kao_image["png"]]
        for tile_index, tile in enumerate(chip_entry["tiles"]):
            self.assertEqual(tile["tile"], tile_index)
            self.assertEqual((tile["original_width"], tile["original_height"]), (16, 16))
            self.assertEqual((tile["hd_width"], tile["hd_height"]), (64, 64))
            self.assertEqual(tile["mode"], "RGBA")
            self.assertEqual(
                tile["png"], f"PORTCHIP/tiles/000_{tile_index:03d}.png"
            )
            self.assertNotIn("\\", tile["png"])
            self.assertEqual(
                tile["sha256"], sha256((self.hd / tile["png"]).read_bytes())
            )
            paths.append(tile["png"])
        self.assertEqual(len(paths), len(set(paths)))
        chip_non_image = portchip["entries"][1]
        self.assertEqual(chip_non_image["note"], "非芯片数据，原样保留")
        self.assertEqual(chip_non_image["tile_count"], 0)
        self.assertEqual(chip_non_image["tiles"], [])
        self.assertNotIn("sheet", chip_non_image)
        self.assertNotIn("sheets", json.dumps(manifest, ensure_ascii=False))

    def test_rejects_wrong_hd_size_before_writing(self):
        self._save_png("KAO/portraits/000.png", (7, 12))
        output = self.hd / "runtime-manifest.json"
        with self.assertRaisesRegex(ValueError, "HD 尺寸"):
            export_runtime_manifest(self.original, self.hd, self.game, output)
        self.assertFalse(output.exists())

    def test_rejects_non_rgba_png(self):
        self._save_png("KAO/portraits/000.png", (8, 12), mode="RGB")
        with self.assertRaisesRegex(ValueError, "必须为 RGBA"):
            self._export()

    def test_rejects_duplicate_entry(self):
        for root in (self.original, self.hd):
            path = root / "KAO" / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["entries"][1] = copy.deepcopy(manifest["entries"][0])
            path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "条目 0 重复"):
            self._export()

    def test_rejects_duplicate_tile(self):
        for root in (self.original, self.hd):
            path = root / "PORTCHIP" / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            tiles = manifest["entries"][0]["tiles"]
            tiles[1] = copy.deepcopy(tiles[0])
            path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "原始芯片 0 重复"):
            self._export()

    def test_rejects_duplicate_png_path(self):
        for root in (self.original, self.hd):
            path = root / "PORTCHIP" / "manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["entries"][0]["tiles"][1]["png"] = "tiles/000_000.png"
            path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "PNG 路径重复"):
            self._export()

    def test_rejects_original_source_hash_mismatch(self):
        (self.game / "KAO.LZW").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "KAO.LZW 的 SHA256"):
            self._export()


if __name__ == "__main__":
    unittest.main()
