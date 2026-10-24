#!/usr/bin/env python3
"""生成雨天"玻璃上挂水珠"材质贴图（批 9）。

纯程序化绘制，无外部美术资源。用法：
    python3 make-water-drops.py [输出目录]
默认输出到脚本所在目录。

每颗水珠三层明暗（裁决硬要求，缺一不合格）：
  1. 上缘高光  白 α 0.85–0.95，1–2px 弧
  2. 内部偏移亮点（伪折射）白 α 0.30–0.40，圆心偏左上 30%
  3. 下缘暗边  深蓝 rgba(10,30,72) α 0.30–0.40，1px 柔边
外圈柔软过渡，无硬边圆点；3×3 平铺拷贝保证无缝。
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

SS = 2           # 超采样
SIZE = 1024
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def _tile(draw_fn, seed, count, dmin, dmax, blur, out):
    """在 3×3 平铺区域内画水珠，再裁出中央格。"""
    rnd = random.Random(seed)
    base = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    dark = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    bd, dd = ImageDraw.Draw(base), ImageDraw.Draw(dark)
    for _ in range(count):
        x, y = rnd.random() * SIZE, rnd.random() * SIZE
        dia = dmin + rnd.random() * (dmax - dmin)
        r = dia * SS / 2.0
        for ox in (-SIZE, 0, SIZE):
            for oy in (-SIZE, 0, SIZE):
                cx, cy = x * SS + ox * SS, y * SS + oy * SS
                box = [cx - r, cy - r, cx + r, cy + r]
                # 柔和的珠体（很低 α，只给一点点折射色，避免硬圆点）
                bd.ellipse(box, fill=(205, 226, 255, 16))
                # 1. 上缘高光白弧
                hi_a = int(255 * (0.85 + rnd.random() * 0.10))
                bd.arc(box, 205, 335, fill=(255, 255, 255, hi_a),
                       width=max(1, int(1.4 * SS)))
                # 2. 内部偏移亮点（圆心左上 30%）
                ir = max(1.0, r * 0.26)
                ix, iy = cx - r * 0.30, cy - r * 0.30
                in_a = int(255 * (0.30 + rnd.random() * 0.10))
                bd.ellipse([ix - ir, iy - ir, ix + ir, iy + ir],
                           fill=(255, 255, 255, in_a))
                # 3. 下缘暗边（单独一层，稍后模糊 1–2px）
                dk_a = int(255 * (0.30 + rnd.random() * 0.10))
                dd.arc(box, 25, 155, fill=(10, 30, 72, dk_a),
                       width=max(1, int(1.0 * SS)))
    dark = dark.filter(ImageFilter.GaussianBlur(1.4 * SS))
    img = Image.alpha_composite(base, dark)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    if blur > 0:
        img = img.filter(ImageFilter.GaussianBlur(blur))
    img.save(out)


def make_mist(out):
    """极淡竖向雾条（α 0.06–0.10），#cfe0ff 色系，可平铺。"""
    size = 512
    rnd = random.Random(20261025)
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for _ in range(260):
        x = rnd.random() * size
        w = 1 + rnd.random() * 1.5
        h = 30 + rnd.random() * 120
        y = rnd.random() * size
        a = int(255 * (0.06 + rnd.random() * 0.04))
        for oy in (-size, 0, size):
            for ox in (-size, 0, size):
                d.line([(x + ox, y + oy), (x + ox, y + oy + h)],
                       fill=(207, 224, 255, a), width=max(1, int(w)))
    img = img.filter(ImageFilter.GaussianBlur(0.8))
    img.save(out)


_tile(None, 20261024, 320, 3, 14, 0.0,
      os.path.join(OUT, "water-drops-near.png"))
_tile(None, 20261025, 180, 2, 7, 1.0,
      os.path.join(OUT, "water-drops-far.png"))
make_mist(os.path.join(OUT, "rain-mist.png"))
print("generated water-drop tiles in", OUT)
