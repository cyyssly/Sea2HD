# Sea2HD —— 《大航海时代2》高清化定制分支

本仓库是 [DOSBox Staging](https://github.com/dosbox-staging/dosbox-staging) v0.83.0 的定制分支，
为 1994 年的 DOS 游戏《大航海时代2》（大航海時代II / Uncharted Waters: New Horizons，KOEI）增加一路
“原味高清”渲染：在游戏自身的 640x480 帧缓冲之上，把关键美术资源用 4 倍分辨率的 PNG 覆盖绘制，
其余像素（对话框、是/否框、鼠标指针、文字位置）**保持游戏原始像素**，不改变任何玩法与数值。

> 仓库内**不包含**任何游戏原作文件或由原作美术生成的高清资源。
> 运行需要你自己拥有的正版游戏，资源由 `sea2hd/tools/` 中的脚本从原作数据生成。

---

## 1. 效果

- **头像（KAO 64x80）**：人物选择、对话、港口人物等界面中的头像，以 256x320（4x）高清 PNG 叠加显示。
- **繁体字**：游戏原本用 16x14 繁体点阵，重绘为简体字形，字号与排版完全沿用原版网格。
- **其余一切原样**：对话框边框、头像框、是/否确认框、鼠标指针、光标下的原始像素均不改动。

## 2. 实现方式

### 2.1 渲染钩子

`render.cpp` 在 `PixelFormat::BGRX32_ByteArray` 且分辨率恰为 640x480 时启用 Sea2HD 通道：
每行回调复制到缓存，帧末调用 `SEA2_HD_Composite()`。合成时先把 640x480 帧缓冲
按最近邻放大 4 倍铺满 2560x1920，再用 RGBA 资源覆盖；被“覆盖”的像素保留游戏原始像素。

### 2.2 头像识别

游戏通过 FAR 调用进入两个固定入口（`0x7cdd` = KAO64，`0x7d4c` = KAO48）来绘制头像。
`sea2_hd.cpp` 在 CPU 层（`core_normal.cpp`）捕获入口，记录调用参数（图号、目标 x/y），
再由两套**完全隔离**的算法决定这一帧是否替换：

| 场景 | 算法 | 说明 |
| --- | --- | --- |
| 新游戏选人界面的 6 个固定框 | `scene_box_shows_portrait()` | 每帧与基线“相邻像素相等/不等”模式比对，稳定 3 帧后锁定 |
| 对话框里的头像窗口 | `dialog_box_shows_portrait()` | 单次 blit 即成画，结构完整即立刻替换 |

两套算法互不调用（除选人场景退场时把框交还给通用算法），并且都建立在同一份
`preload_kao64_assets()` 预加载资源之上。

识别按“相邻像素关系”而不是绝对颜色比较：屏幕淡入淡出只是整体亮度变换，模式不变；
换一张图、清空方框、重绘背景都会立刻破坏模式。另有像素级比对（亮度拟合后）用来识别
对话框、标签、窗口、鼠标指针叠在头像上的局部区域——只有这些被标记的 4x4 块保留游戏像素。

### 2.3 字形重绘

`sea2_glyph_map.h` 保存 `1.PAT` 字形记录到 Unicode 的映射（繁体 / 简体各一套）。
运行时捕获字形读取位置，按记录号取简洁字形位图，写回同一 16x14 单元，
因此文字排版、光标位置、逐字出字节奏都与原版一致。鼠标停在文字上时，
`bios_keyboard.cpp` / `mouseif_dos_driver.cpp` 提供指针位置，用于让指针覆盖的格子保持原始像素。

### 2.4 选人界面的鼠标处理

DOS 鼠标是“软件光标”，由驱动直接画进游戏帧缓冲。若进入选人界面时指针正压在某个头像上，
该头像的像素会被污染。处理办法是：识别到选人界面后调用
`MOUSEDOS_SuppressCursorUntilMove()`——把指针从帧缓冲擦除（先确认帧缓冲里确实是本驱动画的光标，
避免把旧屏幕背景写回画面），把 DOS 指针位置归位到画面右下角并保持不可见；
宿主鼠标真正移动后自动恢复显示，效果等同于“用回车进入该界面”。
驱动的 show/hide 计数不被改动，游戏自身的显示/隐藏平衡保持原状。

## 3. 仓库结构

```
SEA2HD.md                 本文档
src/misc/sea2_hd.cpp      高清合成核心（识别、覆盖、字形、文字悬停、指针联动）
src/misc/sea2_hd.h        对外接口
src/misc/sea2_glyph_map.h 字形记录 -> Unicode 映射表
src/gui/render/render.cpp 渲染管线挂接（4x 输出、帧缓存）
src/cpu/core_normal.cpp   载入段内的入口/字形地址捕获（诊断）
src/dos/dos.cpp           DOS 调用与文件读取追踪（诊断）
src/dos/dos_execute.cpp   程序启动/结束通知（载入段、状态复位）
src/ints/bios_keyboard.cpp 键盘路径追踪（诊断）
src/hardware/input/mouse.h / mouseif_dos_driver.cpp
                          指针位置查询、光标抑制与右下角归位
src/misc/CMakeLists.txt / meson.build  构建挂接
sea2hd/runtime/           实际部署用的启动脚本与 conf 快照
sea2hd/tools/             资源生成脚本（从原作数据导出 4x PNG）
sea2hd/tests/             资源校验与文字悬停测试脚本
```

## 4. 构建

Windows + MSVC（测试环境：Visual Studio 18 BuildTools / MSVC 19.50、Windows SDK 10.0.26100），
依赖由 vcpkg 提供（`SDL2`、`SDL2_image` 等，见 `vcpkg.json`）。

```powershell
cmake -S . -B build/release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build/release
```

产物为 `build/release/dosbox.exe`。Linux / macOS 可用仓库自带的 meson 流程，本分支的改动
已同步写入 `meson.build`。

## 5. 运行

```
dosbox.exe --conf sea2-hd.conf --nolocalconf
```

`sea2hd/runtime/` 是当前部署使用的三份文件快照：

- `大航海时代2(高清).bat`：计算窗口位置/尺寸后启动 `hd\dosbox-staging\dosbox.exe`；
- `sea2-hd.conf`：挂载 `..\sea2`（原作目录）并执行 `play`；
- `sea2-hd-runtime.conf`：挂载 `runtime-game`（分发用目录）并执行 `play`。

启用高清的关键配置：

```ini
[render]
sea2_hd = on
sea2_hd_path = ../assets/hd    ; 高清资源根目录，相对 conf 所在目录
aspect = auto
shader = sharp
integer_scaling = off
image_adjustments = off
```

`[cpu] core = normal` 与 `cpu_cycles = 10500` 是实测能让该游戏稳定跑满、且让入口捕获可靠的组合。

## 6. 资源目录（不在本仓库）

```
assets/
  original/                 从原作解包的低分辨率原始数据（KAO/PORTCHIP/1.PAT 等）
  hd/
    runtime-manifest.json   运行时清单
    simplified_atlas.png    简体字形图集
    palettes/               调色板
    KAO/
      portraits/            KAO64 头像 256x320 4x PNG
      sheets/               KAO48 48x48 -> 192x192
      discoveries/          发现物图标
      items/                物品图标
      manifest.json
    PORTCHIP/               港口芯片图
```

对应生成脚本：

| 脚本 | 用途 |
| --- | --- |
| `sea2hd/tools/koei_assets.py` | KAO.LZW / PORTCHIP.LZW 解包、PNG 导出与回封、清单与图集 |
| `sea2hd/tools/koei_assets_extra.py` | 补充解包：LS10/LS11 归档、PORTMAP、2.PAT 切片、DAT 偏移表容器 |
| `sea2hd/tools/kao48_repaint.py` | 48x48 图标的 4x 重绘流水线（准备参考图 / 校验并归一化） |

脚本默认以 `assets/` 与 `tools/` 位于同一项目根目录，且 `Pillow` 可用。

## 7. 诊断

- `sea2-hd-diag.log`：`sea2-hd.conf` 同目录，记录 `ADD` / `SCENE LOCK` / `HIDE pointer…` /
  每框 `covered`、`diff` 等事件，用于确认识别与替换是否干净。
- `sea2-box-*.png`：调试构建会把每个被跟踪的方框导出为 `src` / `ref` / `mask` 三张图
  （这些文件不入库）。
- `sea2-debug.conf` / `sea2-trace.conf`：打开 CPU 载入段追踪与 DOS 调用追踪的配置，仅在排查时使用。

## 8. 已知限制

- 只支持 640x480 BGRX32 帧缓冲；其它分辨率/格式下自动退回 DOSBox 原生渲染。
- 只处理 KAO64（64x80）与 KAO48（48x48）头像及 16x14 字形，其余图形不改动。
- 识别依赖“相邻像素关系”这一稳定特征；如未来出现某张图自身大面积单色、与背景无法区分的极端情况，
  该框会退回原始像素（而不是错误覆盖）。
- 高清资源需自行生成，且不得随仓库分发。

## 9. 许可

本分支沿用上游 DOSBox Staging 的 **GPL-2.0-or-later**（见 `COPYING`）。
游戏本体与美术资源版权归 KOEI TECMO 所有，本仓库不包含这些内容。
