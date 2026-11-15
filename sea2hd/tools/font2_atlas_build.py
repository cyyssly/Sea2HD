"""生成 2.PAT 的 record → Big5/Unicode 映射表与 4 倍黑体图集。

2.PAT 是 16x14 / 28 字节字形的字库（425 扇区 x 18 = 7650 槽），存放 Big5 二级字：
第 n 槽 == 二级字第 n+1 个字，码位 0xC941..0xF9D4。本脚本按该映射逐槽用 SimSun
15px 重绘做 bbox 对齐 IoU 校验，再用 SimHei 50px 画出 64x56 瓦片。

产出：
  sea2hd/tools/sea2_font2_map.csv    record / level2_index / char / big5 / unicode / iou
  assets/hd/font2_atlas.png          4096x6720，64 列 x 56 像素高，7650 个瓦片

用法：python font2_atlas_build.py
"""
import os, sys, statistics
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
from PIL import Image, ImageDraw, ImageFont

ROOT   = r'd:\OldGames\Sea2HD'
PAT2   = os.path.join(ROOT, 'sea2', '2.PAT')
CSV    = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sea2_font2_map.csv')
ATLAS  = os.path.join(ROOT, 'assets', 'hd', 'font2_atlas.png')
REP    = os.path.join(ROOT, '.screenshots', '_pat2_atlas.txt')

SECTOR, PER_SEC, REC_SIZE = 512, 18, 28
CELL_W, CELL_H = 16, 14
SRC_FONT, SRC_SIZE = r'C:\Windows\Fonts\simsun.ttc', 15
DST_FONT, DST_SIZE = r'C:\Windows\Fonts\simhei.ttf', 50
TILE_W, TILE_H, COLS = 64, 56, 64
DELTA = 1

buf = []
def w(s=''):
    buf.append(s); print(s)

data = open(PAT2, 'rb').read()
NSLOT = len(data) // SECTOR * PER_SEC

def level2():
    out = []
    for hi in range(0xC9, 0xFA):
        for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            code = (hi << 8) | lo
            if not (0xC940 <= code <= 0xF9D5):
                continue
            try:
                ch = bytes([hi, lo]).decode('big5')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.append((code, ch))
    return out

tbl = level2()
w('二级字 %d 个, 2.PAT %d 槽, delta=+%d -> 码位 0x%04X..0x%04X'
  % (len(tbl), NSLOT, DELTA, tbl[DELTA][0], tbl[NSLOT - 1 + DELTA][0]))

def slot_bits(rec):
    off = (rec // PER_SEC) * SECTOR + (rec % PER_SEC) * REC_SIZE
    v = 0
    for i in range(REC_SIZE):
        v = (v << 8) | data[off + i]
    return v

def iou(a, b):
    u = (a | b).bit_count()
    return 0.0 if u == 0 else (a & b).bit_count() / u

_bb = {}
def bbox_of(rec, v):
    if rec not in _bb:
        xs, ys = [], []
        for i in range(CELL_W * CELL_H):
            if v & (1 << (CELL_W * CELL_H - 1 - i)):
                xs.append(i % CELL_W); ys.append(i // CELL_W)
        _bb[rec] = (min(xs), min(ys), max(xs), max(ys)) if xs else None
    return _bb[rec]

def pack(img):
    px = img.load(); v = 0
    for y in range(CELL_H):
        for x in range(CELL_W):
            v = (v << 1) | (1 if px[x, y] >= 128 else 0)
    return v

_src = ImageFont.truetype(SRC_FONT, SRC_SIZE)
_gc = {}
def glyph_of(ch):
    if ch not in _gc:
        img = Image.new('L', (SRC_SIZE * 2, SRC_SIZE * 2), 0)
        ImageDraw.Draw(img).text((SRC_SIZE // 2, SRC_SIZE // 2), ch, fill=255, font=_src)
        bb = img.getbbox()
        _gc[ch] = img.crop(bb) if bb else None
    return _gc[ch]

def aiou(rec, truth, ch):
    bb = bbox_of(rec, truth); g = glyph_of(ch)
    if not bb or g is None:
        return 0.0
    x0, y0, x1, y1 = bb
    tw, th = x1 - x0 + 1, y1 - y0 + 1
    best = 0.0
    for sx in (0.95, 1.0, 1.05):
        for sy in (0.95, 1.0, 1.05):
            nw, nh = max(1, int(round(tw * sx))), max(1, int(round(th * sy)))
            canvas = Image.new('L', (CELL_W, CELL_H), 0)
            canvas.paste(g.resize((nw, nh), Image.LANCZOS),
                         (x0 + tw // 2 - nw // 2, y0 + th // 2 - nh // 2))
            best = max(best, iou(truth, pack(canvas)))
    return best

recs = []
for a in range(NSLOT):
    t = slot_bits(a)
    j = a + DELTA
    if not t or j >= len(tbl):
        continue
    recs.append((a, j, tbl[j][0], tbl[j][1], aiou(a, t, tbl[j][1])))

vals = [r[4] for r in recs]
w('全库 n=%d mean=%.4f' % (len(vals), statistics.mean(vals)))
for lo, hi, name in ((0, 683, '前段'), (684, 3039, '中前'), (3040, 3230, '船名段'),
                     (3231, 6084, '中后'), (6085, 7649, '后段')):
    seg = [r[4] for r in recs if lo <= r[0] <= hi]
    if seg:
        w('  %-6s n=%4d mean=%.4f' % (name, len(seg), statistics.mean(seg)))
w()

with open(CSV, 'w', encoding='utf-8') as f:
    f.write('record\tlevel2_index\tchar\tbig5\tunicode\tiou\n')
    for a, j, code, ch, v in recs:
        f.write('%d\t%d\t%s\t0x%04X\tU+%04X\t%.4f\n' % (a, j, ch, code, ord(ch), v))
w('映射表: %s' % CSV)

rows = (NSLOT + COLS - 1) // COLS
sheet = Image.new('L', (COLS * TILE_W, rows * TILE_H), 0)
d = ImageDraw.Draw(sheet)
font = ImageFont.truetype(DST_FONT, DST_SIZE)
drawn = 0
for a, j, code, ch, v in recs:
    tx = (a % COLS) * TILE_W
    ty = (a // COLS) * TILE_H
    d.text((tx + TILE_W // 2, ty + TILE_H // 2), ch, fill=255, font=font, anchor='mm')
    drawn += 1
sheet.save(ATLAS)
w('图集: %s  %dx%d  %d 瓦片  %.2f MB'
  % (ATLAS, sheet.width, sheet.height, drawn, os.path.getsize(ATLAS) / 1048576.0))

open(REP, 'w', encoding='utf-8').write('\n'.join(buf))
