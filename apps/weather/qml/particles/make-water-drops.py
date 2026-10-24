#!/usr/bin/env python3
"""生成雨天"玻璃上挂水珠"材质贴图（批 9；R107 CPU 优化版）。

纯程序化绘制，无外部美术资源。用法：
    python3 make-water-drops.py [输出目录]
默认输出到脚本所在目录。

每颗水珠三层明暗（裁决硬要求，缺一不合格）：
  1. 上缘高光  白 α 0.85–0.95，1–2px 弧
  2. 内部偏移亮点（伪折射）白 α 0.30–0.40，圆心偏左上 30%
  3. 下缘暗边  深蓝 rgba(10,30,72) α 0.30–0.40，1px 柔边
外圈柔软过渡，无硬边圆点；3×3 平铺拷贝保证无缝。

R107（CPU）：
  - 贴图 1024² → 512²，点数按面积同密度减半（近 320→160、远 180→90）。
  - 远层、雨幕烘焙进同一张 water-drops.png（远层按 0.35/0.55 预乘 α，
    雨幕按 0.08/0.55 预乘 α），QML 只剩 1 个全屏 Image 层（原 3 层）。
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

SS = 2           # 超采样
SIZE = 512       # 1024² → 512²（同密度：点数按面积 ×1/4）
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

NEAR_OPACITY = 0.55   # QML 近层基准 opacity（合并后的唯一一层）
FAR_OPACITY = 0.35    # 原远层基准 opacity → 预乘 FAR_OPACITY/NEAR_OPACITY
MIST_OPACITY = 0.08   # 原雨幕层 opacity → 预乘 MIST_OPACITY/NEAR_OPACITY


def _tile(seed, count, dmin, dmax, blur):
    """在 3×3 平铺区域内画水珠，再裁出中央格；返回 RGBA Image。"""
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
    return img


def make_mist():
    """极淡竖向雾条，可平铺；α 预乘 1/NEAR_OPACITY 使上屏仍为 0.06–0.10。"""
    rnd = random.Random(20261025)
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for _ in range(65):                      # 260/1024² → 65/512²（同密度）
        x = rnd.random() * SIZE
        w = 0.5 + rnd.random() * 0.8
        h = 15 + rnd.random() * 60
        y = rnd.random() * SIZE
        a = int(255 * (0.06 + rnd.random() * 0.04) / NEAR_OPACITY)
        for oy in (-SIZE, 0, SIZE):
            for ox in (-SIZE, 0, SIZE):
                d.line([(x + ox, y + oy), (x + ox, y + oy + h)],
                       fill=(207, 224, 255, a), width=1)
    return img.filter(ImageFilter.GaussianBlur(0.4))


def _scale_alpha(img, factor):
    """整图 alpha 预乘 factor（用 putalpha 保持颜色不变）。"""
    alpha = img.getchannel("A").point(lambda v: int(round(v * factor)))
    out = img.copy()
    out.putalpha(alpha)
    return out


near = _tile(20261024, 160, 3, 14, 0.0)      # 原 320/1024² → 160/512²
far = _tile(20261025, 90, 2, 7, 1.0)         # 原 180/1024² → 90/512²
far = _scale_alpha(far, FAR_OPACITY / NEAR_OPACITY)
mist = _scale_alpha(make_mist(), 1.0)        # α 已按 1/NEAR_OPACITY 预乘

# 自下而上合成：雨幕 → 远层 → 近层（与原三层叠放顺序一致）
merged = Image.alpha_composite(Image.alpha_composite(mist, far), near)
merged.save(os.path.join(OUT, "water-drops.png"))
print("generated water-drops.png: size=%dx%d near=%d far=%d mist=%d"
      % (SIZE, SIZE, 160, 90, 65))
