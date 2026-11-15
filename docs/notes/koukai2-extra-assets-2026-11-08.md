# 《大航海时代2》补充美术资源解包记录

> 记录日期：2026-11-08
> 范围：在既有 `tools/koei_assets.py`（只解 KAO.LZW / PORTCHIP.LZW）之外，
> 对 `sea2/` 里其余美术数据做**无损解包 + 部分可视化**。
> 本文只记录**已实测验证**的结论；推断与未解项一律标注。
> 产物目录：`extracted-2026-11-08\`（原位于旧工作目录 `hd\` 下，未随项目迁移）。
> 工具：`tools/koei_assets_extra.py`（新增，不改动原工具）。

> **路径校正（2026-11-11）**：项目已从 `D:\OldGames\大航海时代2\hd\` 迁至 `D:\OldGames\Sea2HD\`；文中 `hd\` 即旧工作目录（现在的项目根），`hd\runtime-game` 已改名为 `sea2`。本轮产物目录 `extracted-2026-11-08\` 未随项目迁移，下文保留为历史路径。

---

## 1. 结论摘要

| 项目 | 状态 |
| --- | --- |
| LS10 与 LS11 共用同一 LZ 内核与 256 字节字典 | **已验证**，9 个 LS10 文件全部解压成功且长度与 FAT 声明一致 |
| LS10 FAT 条目数公式 `n = (first_offset − 276) / 12` | **已验证**（连续性与越界校验全过） |
| 14 个 `.LZW` 全部逐条无损解压（共 659 条） | **已完成**，逐条 sha256 核对 |
| PORTMAP.LZW = 101 张 96×96 8bpp 线性图块 | **已验证**，拼接后可见海岸线与成片城镇 |
| DAT 偏移表容器（GRAPH/GRAPH2/ENDGRP/EVENT0..6） | **已验证并无损拆分**，共 158 块 |
| `2.PAT` = 7650 个 28 字节 16×14 汉字（Big5 二级字） | **已验证**：`off = (record//18)*512 + (record%18)*28`；第 n 槽 = 二级字第 (n+1) 个，码位 0xC941..0xF9D4 |
| 各归档内部图形编码（RLE / 位平面） | **未解**，只提供无损解压后的原始字节 |

---

## 2. LZW 格式：LS10 与 LS11 的关系

既有工具 `tools/koei_assets.py` 只认 `LS11`。实测 `sea2/*.LZW` 里还有 9 个 `LS10`：

```
LS11 : DATA1.LZW, KAO.LZW, PORTCHIP.LZW, PORTMAP.LZW, WORLDMAP.LZW
LS10 : CHAR.LZW, IAE1.LZW, IAP1..6.LZW, OPGRAPH.LZW
```

**两者共用**：16 字节文件头、偏移 `16..272` 的 256 字节置换字典、
变长码（unary 前缀 + 定长后缀）编码的「字典下标 / 回溯距离 / 匹配长度」，
解压逻辑可原样复用 `LS11Archive.decompress()`。

**差别只在 FAT 结束标记**：

| | 起始 | 每条 | 结束标记 |
| --- | --- | --- | --- |
| LS11 | 272 | 12 字节 `(comp_size, uncomp_size, offset)` 全大端 | 一条 12 字节全零 |
| LS10 | 272 | 同上 | **仅 4 字节零** |

LS10 没有显式条目数，用第一条数据的 `offset`（位于 `272+8`）反推：

```
n = (first_offset - 276) / 12
```

实测：`CHAR=7`、`IAE1/IAP1..6=35`、`OPGRAPH=14`。9 个文件全部满足
「FAT 尾部 4 字节全零 / 条目 offset 连续 / 数据不越界」。
`OPGRAPH.LZW` 的 FAT 之后还有 **29659 字节**未列入 FAT（manifest 记 `trailing_bytes_outside_fat`），
性质未定，已保留整文件 sha256。

---

## 3. 各归档解压结果（本轮全部完成）

| 归档 | 格式 | 条目数 | 解压后特征 |
| --- | --- | --- | --- |
| KAO.LZW | LS11 | 256 | 1920 B（64×80）/ 864 B（48×48），既有工具已导出 PNG |
| PORTCHIP.LZW | LS11 | 14 | 30720 B = 240 个 16×16 4bpp 瓷砖，既有工具已导出 |
| **PORTMAP.LZW** | LS11 | **101** | 每条恰好 9216 = **96×96** 字节，8bpp 线性索引 |
| WORLDMAP.LZW | LS11 | 3 | 21973–25394 B，开头是 16 位小端递增行偏移表，后续 RLE 未解 |
| DATA1.LZW | LS11 | 19 | 2 / 4320 / 9216 / 23040 / 28406 / 32768 B 等混合 |
| OPGRAPH.LZW | LS10 | 14 | 每条开头是大端 32 位子偏移表（开场图容器），内部未解 |
| IAP1..6 / IAE1 | LS10 | 各 35 | 第 0 条 11408 / 10544 B，其余固定 6896 / 3984 B，疑似精灵帧 |
| CHAR.LZW | LS10 | 7 | 每条固定 5120 B，开头 16 位小端表 |

逐条解压长度与 FAT 声明 100% 一致，无一条报错；产物见 `lzw\<NAME>\bin\*.bin` 与各自 `manifest.json`。

### 3.1 PORTMAP 的可视化

9216 = 96×96，按 8bpp 线性像素直接铺开即得到**结构正确的地图**：
灰度预览里能辨认出海岸线、成片房屋/城镇、白色道路与桥梁。
`lzw/PORTMAP/preview/contact-sheet.png`（10 列 × 11 行）拼起来后陆地轮廓跨块连续，
说明这 101 条是同一张（或少数几张）大地图的顺序切块，而不是 101 个互不相关的港口。

**注意**：数值是 8bpp 调色板索引，真实 256 色调色板尚未定位，
预览用「索引值当灰度」，只用于看结构，**不是游戏原始配色**。

---

## 4. DAT 偏移表容器

首 4 字节大端 = 偏移表字节长度，且等于第一块的起始偏移；
表内是连续大端 32 位偏移，按相邻差值切块即可无损拆分。

| 文件 | 块数 | 首块示例宽高（块首 4 字节小端 u16×2） |
| --- | --- | --- |
| GRAPH.DAT | 65 | 144×56 |
| GRAPH2.DAT | 2 | 144×56 / 640×400 |
| ENDGRP.DAT | 59 | 224×160 |
| EVENT1.DAT | 5 | 192×144 |
| EVENT2.DAT | 6 | — |
| EVENT3/4.DAT | 各 5 | — |
| EVENT5.DAT | 6 | — |
| EVENT6.DAT | 4 | — |
| EVENT0.DAT | 1 | — |

共 158 块，全部落在 `dat-containers\<NAME>\blocks\*.bin`。
块体是**未解码**的 RLE / 位平面图形（字节分布高度偏斜、含大量 `0f / f0 / 38 / 56` 等模式字节），
本轮不做解码，避免产出噪声冒充美术图。

---

## 5. `2.PAT`：确认是 16×14 汉字字库，存放 Big5 二级字

`koukai2-hd-font-analysis.md` 曾把 `2.PAT` 记为「16×16、32 字节/字、未分析」，
`koukai2-4x-hqx-runtime-engine-plan.md` 也把它列为「未分析，可用来覆盖 16×16 字形与半角数字」。
本轮逐像素复核后，**两处旧结论均被推翻**，上述两份文档已同步更正，实际结构如下。

### 5.1 格式：与 `1.PAT` 完全同构

- 217600 字节 = **425 扇区 × 512**，每扇区 18 个字形 + 8 字节零填充；
- 每字形 **28 字节**（不是 32），16 列 × 14 行，行优先、MSB-first（`bit = row*16 + col`）；
- 槽位公式与 `1.PAT` 相同：`off = (record // 18) * 512 + (record % 18) * 28`；
- 共 **7650 槽**（425 × 18），全部非空。

### 5.2 字符集：Big5 二级字

- `2.PAT` 第 n 槽 == **Big5 二级字区第 (n+1) 个字**，即码位 **0xC941..0xF9D4**。
- 佐证：Big5 二级字共 7652 个，`2.PAT` 7650 槽，差额恰好是首码 `0xC940` 与尾码 `0xF9D5`。
- 与 `1.PAT` 共用同一套 record 编号，但**同 record 不是同一个字**：`1.PAT` 存 Big5 一级字
  （record 684..6084，5401 个），`2.PAT` 存二级字，两套编号各自独立、互不重叠。
- 该字库正是**船名输入界面**罕用字的来源。

### 5.3 旧结论为何错

上一版按「32 字节 / 16×16」切片，块边界整体错位，展开出的自然是图形而非汉字；
按 28 字节重切后即为结构完整的汉字点阵。此前产出的 `pat2/atlas.png` + `pat2/sheets/`
是错误切片下的可视化，**已作废**，不要再用它去补 `1.PAT` 或做半角数字 HD 化。

### 5.4 现行产物

- `tools/sea2_font2_map.csv`：7650 行 `record → Big5 → Unicode` 映射（附逐字 IoU 置信度）；
- `assets/hd/font2_atlas.png`：按该映射用 SimHei 50px 重绘的 64×56 瓦片，已接入运行时 4 倍文字管线。

---

## 6. 产物与复现

```
extracted-2026-11-08\
├── README.md                 # 目录说明、确定性分级、未解项
├── manifest.json             # 总览（源文件 sha256、文件数、各分类摘要）
├── pat2\                     # atlas.png (1600×1088) + sheets\sheet-00..08.png + manifest.json
├── lzw\<14 个归档>\           # bin\*.bin + manifest.json；PORTMAP 另有 preview\*.png
└── dat-containers\<10 个容器>\ # blocks\*.bin + manifest.json
```

共 **956 个新文件 / 约 5.6 MB**。每个子目录的 `manifest.json` 记录源文件 sha256、
每条目的 offset / 压缩长度 / 解压长度 / 解压后 sha256。

复现（需要带 Pillow 的 Python，本机为 `C:\Users\SLY\AppData\Local\Programs\Python\Python310\python.exe`）：

```bash
"C:/Users/SLY/AppData/Local/Programs/Python/Python310/python.exe" tools/koei_assets_extra.py \
  --output "D:/OldGames/Sea2HD/extracted-<新目录名>"
```

`--output` 已存在时工具**直接拒绝运行**；所有写入都用「文件不存在才写」的保护，
因此不会覆盖已有资源。本轮前后对旧工作目录（`hd\`，即现在的项目根）顶层做了 sha256 + mtime 快照比对：
原有 122 个文件、4 个目录**全部未变**，唯一新增顶层条目是 `extracted-2026-11-08`。

---

## 7. 未完成 / 下一步候选

1. **PORTMAP 调色板**：从 MAIN.EXE 的 VGA DAC 写入代码或运行时截帧取 256 色，
   把灰度预览换成真实配色；再按地图实际宽高重排（当前 10 列只是便于查看的拼接）。
2. **DAT 块体解码**：已知块首宽高，需定位解压/RLE 例程才能出 PNG；
   GRAPH2 第二块 640×400 是全屏图，优先级最高。
3. **WORLDMAP / OPGRAPH / IAP / IAE / CHAR** 的内部结构（16 位行表、大端子偏移表、
   8 字节精灵帧头）已能看到轮廓，编码未解。
4. ~~`2.PAT` 定性~~ **已完成**：确认为 16×14 汉字字库，存放 Big5 二级字，见第 5 节。
5. 半角数字 / ASCII 的 HD 化仍未做；`2.PAT` 与此无关（其中不含半角字形）。

> 本轮未改动：`sea2\` 任何文件、`tools/koei_assets.py`、`assets/original`、`assets/hd`、
> 以及项目根目录下原有的 PNG 与配置。
