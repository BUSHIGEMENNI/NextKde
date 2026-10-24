#!/usr/bin/env python3
"""生成天气 app 的可平铺雨点材质贴图（批 8）。

纯程序化绘制，无外部美术资源。用法：
    python3 make-rain-material.py [输出目录]
默认输出到脚本所在目录。
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

SS = 2           # 超采样，导出时降采样做抗锯齿
SIZE = 512
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def make(path, count, dmin, dmax, amin, amax, tail_ratio, blur):
    rnd = random.Random(20261024)
    img = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    color = (207, 224, 255)  # #cfe0ff
    for _ in range(count):
        x, y = rnd.random() * SIZE, rnd.random() * SIZE
        dia = dmin + rnd.random() * (dmax - dmin)
        alpha = int(255 * (amin + rnd.random() * (amax - amin)))
        radius = dia * SS / 2
        # 20% 近层雨点带 1–2px 短拖尾，避免全是圆点
        trail = (1.0 + rnd.random()) * SS if rnd.random() < tail_ratio else 0
        # 3×3 平铺拷贝，保证四边无缝
        for ox in (-SIZE, 0, SIZE):
            for oy in (-SIZE, 0, SIZE):
                cx, cy = x * SS + ox * SS, y * SS + oy * SS
                if trail > 0:
                    draw.line([(cx, cy), (cx + trail * 0.35, cy + trail)],
                              fill=color + (max(0, alpha // 3),),
                              width=max(1, int(radius)))
                draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius],
                             fill=color + (alpha,))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    if blur > 0:
        img = img.filter(ImageFilter.GaussianBlur(blur))
    img.save(path)


make(os.path.join(OUT, "rain-material-near.png"), 420, 2, 9, 0.25, 0.85, 0.20, 0.4)
make(os.path.join(OUT, "rain-material-far.png"), 260, 1, 4, 0.10, 0.35, 0.10, 0.9)
print("generated rain-material tiles in", OUT)
