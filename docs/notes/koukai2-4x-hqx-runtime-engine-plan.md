# 《大航海时代2》4 倍 HD 运行时引擎计划（2026-11-06 校正版）

> **路径校正（2026-11-11）**：项目已从 `D:\OldGames\大航海时代2\hd\` 迁至 `D:\OldGames\Sea2HD\`。文中 `hd/` 前缀即当前项目根；测试副本 `hd/runtime-game` 已改名为 `sea2`；原版目录仍指 `D:\OldGames\大航海时代2\sea2`（不修改）。以下路径均已按新结构更新。

> **校正说明**：本文最初写于实施前，基于若干后来被证伪的假设（320×200 分辨率、1280×800 画布、`src/koukai2/hd_*.cpp` 多模块结构、KAO/PORTCHIP 优先级最高）。经过实际逆向与实机验证，这些内容已全部更正。**分析结论与数据以 [`koukai2-hd-font-analysis.md`](./koukai2-hd-font-analysis.md) 为准**，本文保留为"目标 + 实际落地路径"的计划记录。

## Context

用户希望真正使用 4 倍高清素材，而不是对最终画面做滤镜放大。方案确定为**修改 DOSBox Staging 0.83.0**：保留原 DOS 游戏逻辑与逻辑坐标，在**资产绘制阶段**把原始 8bpp 素材替换为 4 倍素材。

现实情况（校正后的关键事实）：

- 游戏**不是** 320×200。中文版启动后进入 **640×480（SVGA）** 图形模式；4 倍合成画布为 **2560×1920**（`render.cpp` 中 `Sea2HdScale = 4`）。
- 汉字字格 **16×14 像素**、点阵 **28 字节/字**，字库为 `1.PAT`（173112 字节，6086 个字形槽）。
- 高清模块**没有**拆成 `hd_bridge` / `hd_assets` / `hd_compositor` 三个文件，实际是单文件 `src/misc/sea2_hd.cpp` + 生成式表头 `src/misc/sea2_glyph_map.h`。
- 素材来源是 `assets/hd`（4 倍 RGBA PNG，含简体字形图集）。
- 不得用 `image_to_pixels()` 把 HQX 量化回原 8/16 色；不得把 LS11 回封成功当作高清显示验证。

## 实际架构

```
                    ┌──────────────────────────────────────────┐
  DOS 游戏 (640×480) │  render.cpp: RENDER_DrawLine 被替换为       │
                    │  sea2_hd_line_handler()                  │
                    └───────────────┬──────────────────────────┘
                                    │
                    ┌───────────────▼──────────────────────────┐
                    │  src/misc/sea2_hd.cpp                    │
                    │  ① DOS 读钩子  → glyph_index[28B]=record  │
                    │  ② blitter 入口 → register_text_cell      │
                    │  ③ 每帧 identify_text_cell (16×14)        │
                    │  ④ draw_text_cell → 图集瓦片 64×56        │
                    │  ⑤ KAO64/KAO48 入口 → HQX 头像合成        │
                    └───────────────┬──────────────────────────┘
                                    │
                    ┌───────────────▼──────────────────────────┐
                    │  assets/hd/simplified_atlas.png (4096×4760)│
                    │  assets/hd/KAO/*.png (4× RGBA)            │
                    └──────────────────────────────────────────┘
```

## 实施记录（按原计划六步校正）

### 1. 建立可回退的引擎构建基线 ✅

- 源码：`dosbox-staging-src`（DOSBox Staging 0.83.0）。
- 真正用于编译的构建目录是 `C:\Users\SLY\AppData\Local\Temp\sea2-dosbox-src\build\release`，它是源码的**独立副本**。改完源码必须手动 `Copy-Item` 到该副本，否则 ninja 报 "no work to do"。
- 编译前必须 `$env:CL='/utf-8'`。
- 原版游戏目录（`D:\OldGames\大航海时代2\sea2`）不修改，所有测试跑 `sea2` 副本；`大航海时代2(高清).bat` 指向 `dosbox-staging/dosbox.exe` + `sea2-hd.conf`。
- 删除早期失败的 `dosbox-staging-src/build`（无 exe 的空 configure），避免误用。

### 2. 素材清单与格式 ❌ 已改道

- 原计划做 `tools/koei_assets.py` 导出"运行时清单"。实际高清管线**不需要运行时清单**：字形走生成的 C++ 常量表（`sea2_glyph_map.h`，5401 条），头像直接按 KAO 入口序号取对应 PNG。
- `tools/koei_assets.py` 仍保留解包/回封能力，`dosbox-staging-src/sea2hd/tests/test_koei_assets_hd.py` 覆盖该工具，但**已删除** `assets/hd-indexed`、`assets/repacked` 等旧实验产物。
- LS11 回封仅作为原格式工具，不参与高清渲染。

### 3. 定位实际绘制接口 ✅

- KAO 的两个包装函数通过**32 字节函数签名**识别，而不是写死线性地址：`Kao64Entry = 0x7CDD`、`Kao48Entry = 0x7D4C`（模块内偏移，配合签名匹配与 exe 版本判定）。
- 文本路径改用**更可靠的钩子组合**：
  1. DOS 读钩子捕获 28 字节字模 → 记录号；
  2. 通用平面 blitter 入口捕获源指针、目标 x/y → 登记文本格；
  3. 每帧从帧缓冲重新识别该格（颜色数 ≤3、墨迹像素 4–150）。
- 关键教训：**不做"逻辑坐标 → 字形"的长期缓存**。早期方案 A 的缓存会被菜单重绘冲掉，导致简体突然退回繁体；改为每帧重识别后问题消失。

### 4. 主机侧 RGBA 高清合成 ✅（改为单文件）

- 合成入口为 `SEA2_HD_Composite()`，输出 4 倍画布；`render.cpp` 在 `sea2_hd_output_active` 时跳过原 scaler，直接按 4 倍尺寸输出。
- `sea2_hd` 配置默认 **off**（`render.cpp:1657`），`sea2_hd_path` 默认 `../assets/hd`；运行时可热切换。
- 未识别区域一律保留原像素，不做整屏替换。

### 5. 覆盖范围 ✅ 字形 / 部分头像（不是全部）

| 素材 | 状态 |
| --- | --- |
| 16×14 汉字（`1.PAT`，记录 684–6084） | ✅ 4 倍简体图集，实机验证 |
| KAO64（64×80 头像） | ✅ 早期完成并实机验证 |
| KAO48（48×48 发现物/物品） | ❌ 入口已定位，合成未实现 |
| PORTCHIP（港口瓷砖） | ❌ 未实现 |
| 港口侧栏固定标签、ASCII 半角数字 | ❌ 未覆盖（不走该 blitter 路径） |

### 6. 配置与交付 ✅

- `sea2-hd.conf`：面向使用（挂 `sea2`，关 HD 时可直接回退）。
- `sea2-debug.conf`：原版对照（挂 `sea2`，不开 HD）。
- `sea2-trace.conf`：诊断输出。
- `sea2-hd-runtime.conf`：高清开启 + 只跑 `sea2` 副本的测试配置。

## 验证

1. **字模映射**：13 个锚点（684=一、699=力、2407=海、2545=航、3508=証、6084=籲 等）全部命中；684–6084 共 5401 个 28 字节位图两两互不相同。
2. **运行时证据链**：日志 `blit x=224 y=272 ann=4720 look=4720 b=201011FF...` 与 `1.PAT` 物理记录 4720 逐字节一致；新表 4720 = 請/请，修正后用户实机确认错字大量修复。
3. **图集**：`idx = record − 684` 的瓦片目视正确（4719 诞、4720 请、4721 诸、4722 课）。
4. **开关回归**：`sea2_hd = off` 时画面与未修改引擎一致。
5. **已废弃的验证手段**：不把 LS11 回封成功、文件成功加载、静态反汇编当作"高清已生效"；不用截图模板匹配或固定坐标冒充通用支持。

## 未完成项

- 段边界（≈4397 / 4784 / 5313 / 5499 / 5583 / 5729）各有 ±1 条记录的映射不确定性；`1.PAT` 记录 6085 用途未定。
- 「知力 → 知证」缺少 `look=3508` 运行时日志证据。
- 只有走该 blitter 的文本格被 HD 覆盖；存档列表等画面仍为原字库。
- `2.PAT` 已定性为 Big5 二级字字库（7650 槽 × 28 字节 16×14，第 n 槽 = 二级字第 n+1 个，码位 0xC941..0xF9D4），HD 图集 `assets/hd/font2_atlas.png` 已接入；数字/半角仍未 HD 化。
- KAO48、PORTCHIP、港口侧栏标签均未支持。

## 硬性约束

- 原版游戏目录与存档不得修改，测试只用 `sea2` 副本。
- 密码页只发送空白回车/鼠标点击，绝不输入任何文本。
- 不得宣称 KAO48、PORTCHIP 或全部素材已支持。
- 自动化测试：启动必须 `Start-Process -WorkingDirectory 'D:\OldGames\Sea2HD'` + `--conf <绝对路径>` + `--nolocalconf`；不要 `MoveWindow`/`ShowWindow`（SDL 软件渲染器会白屏）；按键用 `PostMessage(WM_KEYDOWN/WM_KEYUP)`；截图用 `PrintWindow(hwnd, hdc, 0x2)`；启动/按键/截图必须在同一条 Shell 命令内完成。
