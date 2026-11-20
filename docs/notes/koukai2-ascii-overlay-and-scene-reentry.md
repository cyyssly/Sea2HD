# 数字（ASCII）高清：遮挡、指针与换场景恢复

- 日期：2026-11-14
- 状态：**已生效**
- 代码：`dosbox-staging-src/src/misc/sea2_hd.cpp`

## 一、要解决的四个问题

| 问题 | 现象 |
| --- | --- |
| 穿透 | 右侧面板的数字（810 / 0 / 720 / 0 / 100% 等）画在打开的对话框上面 |
| 指针掉高清 | 鼠标箭头压在数字上，该数字变模糊 |
| `%` 掉高清 | 箭头压在百分号附近时，`%` 退回低清 |
| 换场景恢复慢 | 进入建筑物后先看到原版数字，过一会才变高清 |

## 二、关键事实（排查得到，务必先读）

1. **游戏不是用整块贴图画界面，而是用 8×16 的细条 blit 拼出来的。**
   诊断日志里全是 `PANELBLIT ... w=1 h=16`，单块 8×16 = 128 像素。
   因此"整块美术"的门槛（`CoverMinPixels = 512`）永远拦不到对话框，
   `cover_rect_count` 始终为 0，`COVERRECT` 一条记录都没有。

   > 结论：早期那套"靠 blit 矩形 + 先后次序判断遮挡"的方案在这些界面上**从来没有执行过**。
   > 后来所有有效修复都不依赖 `cover_rect`。

2. **数字格是每帧从帧缓冲重新识别的。**

   - `identify_ascii_cell()` 统计笔画色与纸色；
   - `has_foreign` 表示"字形位置上出现了非笔画色"，即这一格上有东西压着；
   - `identify_ascii_cell_while_covered()` 只在指针压住时作为兜底。

3. **指针和对话框都必须靠"指针框"区分。**
   `inside_pointer_box()` 以指针坐标为基准，外扩 `PointerBoxSlack = 6`，
   框大小 `36×40`。箭头本体远小于这个框。

4. **换场景的唯一可靠信号是整屏全黑**（`screen_is_fully_black()`），
   此时调用 `drop_scene_text_state()`。

## 三、最终方案

### 1. 遮挡：严格相等 + "自己确实露出来了"（2026-11-15 改）

两个函数配合：

- `ascii_cell_cover_mask()`：逐像素比较，**只有严格等于**该格此刻笔画色/纸色的像素才算
  "自己的"，其余标进 `keep` 掩码（全部 128 像素都被标住时返回 `fully_covered`）。
- `ascii_cell_open_counts()`（新增）：统计开放区域——`keep` 之外、指针框之外——里字形位与
  空白位的像素数，`open_ink` / `open_paper` 都必须 ≥ 2 才允许绘制。

绘制循环里：

```cpp
const bool fully_covered = ascii_cell_cover_mask(source, source_width, cell, keep);
...
ascii_cell_open_counts(cell, keep.data(), have_pointer, pointer_x, pointer_y,
                       open_ink, open_paper, open_total);
if (open_total > 0 &&
    (open_ink < AsciiMinOpenInk || open_paper < AsciiMinOpenPaper)) {
    if (cell.drawn) { cell.drawn = false; frame_rebuild = true; }
    continue;   // 整格不画，保留游戏自己的像素
}
```

为什么不再用"可见笔画过半"（`visible * 2 < strokes`，2026-11-14 的做法）：面板纸色和
对话框纸色是同一个颜色（`F3E3D2`），只看"笔画色还剩多少"永远分不出"盖住"和"自己"。
现在的规则改成**墨色和纸色都必须还能看见**：对话框盖住整格时，字形位置上全是纸色，
`open_ink` 必然是 0，这一格就不画。

| 场景 | open_ink | open_paper | 结果 |
| --- | --- | --- | --- |
| 数字完整可见 | 字形位数 | 空位数 | 画高清 |
| 指针压住一部分 | 框外剩余 | 框外剩余 | 画高清，指针像素原样贴回 |
| 整格被同色纸面（对话框）盖住 | 0 | — | 不画 |
| 指针压满整格（open_total = 0） | — | — | 豁免，按 `covered` 规则照画 |

> `keep` 的判定必须严格相等。给一点容差（早期用过 `colour_close`，±8），对话框和按钮就会
> 被当成"自己的像素"，高清数字立刻穿透。

### 2. 换场景：文字格整体清空

`drop_scene_text_state()` 里**不再保留数字格**，`ascii_cells` 整体清空（与
`text_cells` 一致）：

```cpp
void drop_scene_text_state()
{
    text_cells.clear();
    ascii_cells.clear();
    // 上一场景的窗口矩形与绘制次序同样作废
    cover_rect_count = 0;
    blit_order       = 0;
    window_rect_count = 0;
    window_order      = 0;
    ...
}
```

> 历史：早期版本曾逐格重置 `ascii_cells`（只清 `colors_valid` / `seen_this_frame`
> 等标志）以适配"建筑里面板数字只画一次"的情况。现在的遮挡与文字归属改由
> 绘制次序 + 矩形级判定决定（见 `koukai2-graph-pipeline-2026-11-21.md`），
> 不再依赖跨场景保留的格子，因此回到整体清空。

### 3. 指针压住时仍然整格重画 + 逐像素还原

指针路径维持原样：

```cpp
draw_ascii_digit(cell, cell.background, cell.ink, destination,
                 destination_stride, keep.data());
restore_ascii_covered_pixels(...);
if (covered || cell.has_overlay) {
    restore_ascii_pointer_pixels(..., cell.background, cell.ink, /*anywhere=*/true);
}
```

- `covered`：指针框确实压在这一格上，此时即使识别失败也沿用记忆色；
- `has_overlay`：本帧有外来像素，还原时必须在整格逐像素进行，
  只还原指针框会把对话框像素漏掉。

## 四、保留但当前不生效的机制

`CoverRect` 的采样签名（`CoverSampleCols/Rows = 16×16`，失配超过 50% 判定美术已消失）
与 `verify_cover_rects()` 仍然保留。当前游戏的界面全部走细条 blit，
这套机制不会登记任何矩形；一旦将来有整块美术走 blitter（如 KAO 肖像以外的窗口），
它会自动生效，不需要再改。

## 五、调试手段

- 日志：工作目录下 `sea2-hd-diag.log`，每次启动截断。当前保留的是状态机事件：
  `ADD` / `SCENE LOCK` / `HIDE` / `LOCK` / `PREVIEW` / `GIVEUP` / `REFRESH` / `HEAL` /
  `MISS` / `RELOCK` / `UNMASKED` / `MASKED` / `MATCH` / `PORT*` / `SYMBLIT` / `GLYPHMISS` /
  `DOSOPEN` / `FILEIO`。
- 排查数字链路时用过的探针（`ASCIICELL` / `ASCIIFAIL` / `ASCIIPIX` / `ASCIIDRAW` /
  `ASCIISTATE` / `ASCIIMISS` / `ASCIISKIP` / `ASCIINOPE` / `ASCIITRUST` / `ASCIILEFT` /
  `ASCIIRIGHT` / `PANELSCAN` / `PANELHIT` / `PANELBLIT` / `PANELTXT` / `PANELASCII` /
  `KAODONE` / `FADEDONE` / `SCENEBLACK` / `SHOPDRAW` / `COVERRECT` / `TXTHOVER` 等）已在
  验证通过后全部删除，连同 `dump_scene_frame` 的截帧与三个未使用的导出函数。
- `sea2_frames/`：由 `sea2_hd_frame_dump` 打开的整帧导出（见 `SEA2HD.md` §5）。


## 六、排查过程中被否定的假设（避免重走）

| 假设 | 结论 |
| --- | --- |
| `record_cover_rect()` 的 `y` 少了 `ContentYOffset`（整体偏上 40） | 不成立：数字格只有 8×16，整体偏 40 行就根本不会与格子相交 |
| `cell.order <= rect.order` 的先后判据能表达层次 | 不可靠：数字重画后 `order` 刷新，对话框反而被判成"在下面"；`AsciiCell::order` 仍保留，只用于 `CoverRect` 的登记顺序，不再参与数字格的遮挡判定 |
| 对话框底色把 `cell.background` 投票污染 | 不是本问题的主因；曾加过"纸色锁定"，导致读档界面反白行数字变粗，已回退 |
| `draw_ascii_digit` 只画字形位图内像素能消除残影 | 无效：残影不是抗锯齿外圈；该参数已删除 |
| "整格都是面板纸色"= 底色已铺、数字还没落笔，可以照常画 | 2026-11-15 否定：对话框纸色与面板纸色同色，对话框盖住整格时画面完全一样，这条捷径就是穿透的来源（`colour_only` 已删除） |
| 扫描时两种颜色各试一次当笔画色，命中即登记 | 2026-11-15 否定：会登记出 `ink = F3E3D2`（纸色）的反色格，遮挡判定对它完全失效；现在配色已学到时要求笔画色必须是面板墨色 |
| 在合成循环里写 `frame_dirty = true` 就能"再合成一次擦掉残影" | 2026-11-15 否定：函数末尾无条件写 `frame_dirty = false`，请求当场失效；改为独立标志 `frame_rebuild`，末尾才合并 |

## 七、2026-11-15：建筑内"数字穿透、鼠标一动就消失"

### 现象

进入建筑物后右侧面板数字穿透到打开的对话框上面；**鼠标一移动就恢复**。

### 三条硬证据（`sea2-hd-diag.log`）

```
ASCIIFAIL since=28 f=129 x=208 y=128 dig=50 why=noink kg=50 kb=78 gc=1 bc=1 ...
ASCIITRUE f=420 x=592 y=128 dig=48 why=noink
ASCIIDRAW f=420 x=592 y=128 digit=48 ink=000000 bg=F3E3D2 kept=72 painted=117
PANELHIT x=208 y=96 code=50 ink=F3E3D2 paper=000000
```

1. `gc=1 bc=1`：整格只有一种颜色（全纸色），`why=noink` 被 `colour_only()` 拦下并当成识别成功；
2. `ASCIITRUE` → `ASCIIDRAW`：这条捷径直接进入绘制，把高清数字画到对话框上；
3. `PANELHIT ink=F3E3D2`：扫描把纸色当成笔画色登记了反色格。

### 为什么鼠标一动就好

`colour_only()` 只在 `cell.fresh > 0`（blit 后 3 帧内）生效，而 `register_ascii_cell()`
对**已存在的格子也会把 `fresh` 重置为 3**。指针停在面板/对话框上时，游戏为了画箭头会反复
重绘那一小块 8×16 细条，反复命中已登记格子，`fresh` 一直被续命，这条捷径就一直开着。
鼠标移开、不再重绘，`fresh` 归零，下一帧合成按正常规则把格子判掉，穿透于是消失。

### 修复（A→D）

| 项 | 内容 |
| --- | --- |
| A | 绘制前用 `ascii_cell_open_counts()` 要求墨色位与纸色位各 ≥ 2，被同色纸面盖住时 `open_ink = 0` → 整格不画 |
| B | 删除 `colour_only()` 这条"整格纸色 = 已识别"的捷径，认不出来就是不画 |
| C | `scan_panel_digits()` 在面板配色已学到时要求笔画色匹配 `ascii_panel_ink`；配色未知时仍允许扫，否则"整块恢复、没有逐字符 blit"的面板永远学不到配色、永远不扫 |
| D | 循环内改用 `frame_rebuild`，函数末尾 `frame_dirty = frame_rebuild`，让"再合成一次擦掉残留瓦片"真正生效（最多多合成一帧） |

### 验证

进建筑即时高清、对话框打开时无穿透、关掉对话框后数字立即恢复、鼠标停在数字/对话框上无残影。

> 上面的 `PANELHIT` / `ASCIIFAIL` / `ASCIITRUE` / `ASCIIDRAW` 等日志是排查期的探针输出，
> 验证通过后已随 §5 列出的全部临时诊断一起删除；这段记录保留的是当时的直接证据。
