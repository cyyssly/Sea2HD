# 港口地图高清：暂停说明

- 日期：2026-11-13
- 状态：**已暂停**（代码与素材完整保留，仅注释掉调用）

## 决定

暂停"港口地图整体高清化"，只保留港口界面其余部分的高清化：

| 项目 | 状态 |
| --- | --- |
| 港口地图（整图重建 / 偏移跟踪 / 调色板实测 / 逐格绘制） | **停用** |
| 港口界面文字（汉字、符号） | 保留 |
| 港口界面数字 / ASCII | 保留 |
| 左右面板标签（原字形 4 倍平滑放大） | 保留 |
| 港口功能图标（走 1.PAT 符号路径） | 保留 |
| 头像（KAO） | 保留 |

## 为什么停

港口地图不是一张静态贴图，而是 1536×1536 的整图在一个固定 384×384 的窗口里滚动，游戏每帧
用 PORTCHIP 的 16×16 图块现拼可见部分。要高清化就必须：

1. 预先重建整张地图（96×96 格，每格算颜色哈希 + 关系哈希）；
2. 每帧搜索滚动偏移（颜色哈希失配时退到关系哈希）；
3. 每帧从窗口反推"地图色 → 屏幕色"映射（游戏会按昼夜、淡入淡出改写 16 色调色板）；
4. 合成时逐像素校验，任何一处对不上就把该像素交回游戏。

这套逻辑的判定链路很长（颜色哈希保位 → 亮度缩放 → 关系哈希保位 → 面板遮挡 → 渐变保位 →
全图搜索），任何一环判断失误都会表现为"地图闪一下变回低清"或"画面错位"，调参成本极高、
收益只有背景图变清晰。因此先停用，把精力放在文字、数字、面板这类高频可见的地方。

## 停用了什么

全部改动都在 `dosbox-staging-src/src/misc/sea2_hd.cpp`，只是注释掉三处**调用**，
函数本体、常量、状态、素材加载逻辑一律保留：

1. `SEA2_HD_NotifyProgramExecuted()` 里的预加载：

   ```cpp
   if (hd_enabled) {
       preload_kao64_assets();
       // 港口地图高清暂时停用，连预加载一并跳过（恢复时取消注释）。
       // preload_port_maps();
   }
   ```

2. `SEA2_HD_Composite()` 里的每帧更新与绘制：

   ```cpp
   // if (source_width == VgaWidth && source_height == VgaHeight) {
   // 	update_port_map(source, source_width);
   // 	draw_port_map(source, source_width, destination,
   // 	              destination_stride);
   // }
   ```

保留但当前不参与运行的代码：

- `update_port_map()` / `draw_port_map()` / `preload_port_maps()` / `load_port_map()` /
  `load_port_map_art()` / `port_map_index_of()` 等函数；
- `PortMap` 结构、`PortMap*` 常量、`port_maps` / `port_map_art` / `port_map_palette` 等状态；
- `assets/hd/...` 下的 `PORTMAP/*.png`、`PORTCHIP*.PAL` 素材与预加载路径。

## 如何恢复

按顺序取消上面两处注释（共 3 行调用）即可，无需其它改动：

1. 恢复 `preload_port_maps();`
2. 恢复 `if (source_width == VgaWidth && ...)` 整段（`update_port_map` + `draw_port_map`）

恢复后建议先跑一遍港口界面，确认 `sea2-hd-diag.log` 里没有连续的 `PORTMISSLOST` / `PORTMISS`，
再进入建筑、读档、昼夜切换等场景复查。

## 已知问题（停用前的状态，恢复时需要一起处理）

- 在"是/否确认框"（雕刻纹边框）与鼠标指针**同时**压住数字时，数字偶发退回低清。
  边框花纹本身含笔画色像素，任何按颜色比例的判定都会被污染；目前靠"整格画高清 + 逐像素
  还原遮挡像素"缓解，但双重遮挡时仍有漏网。
- 面板标签的 `panel_label_*` 状态与场景切换的清理时机相关，若地图恢复后再次出现
  "旧画面文字残留"，先检查黑屏判定（`screen_is_fully_black()`）是否在该场景命中。

## 相关文件

- 代码：`dosbox-staging-src/src/misc/sea2_hd.cpp`
- 设计说明：`dosbox-staging-src/SEA2HD.md` 第 2.6 节
- 素材对照：`docs/port-tile-hd-compare.png`、`docs/port-tile-hd-gallery.png`
- 港口素材清单：`docs/materials/port2_manifest.json`、`docs/materials/port42_manifest.json`
- 历史排查记录：`docs/notes/koukai2-save-port-locating.md`
