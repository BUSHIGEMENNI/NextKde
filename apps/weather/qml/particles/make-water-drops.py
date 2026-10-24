#!/usr/bin/env python3
"""生成雨天"玻璃上挂水珠"材质贴图（批 10：扁平透镜水痕）。

纯程序化绘制，无外部美术资源。用法：
    python3 make-water-drops.py [输出目录]
默认输出到脚本所在目录。

批 11 修订（取代批 10 的泪滴/拖尾）：所有水珠一律圆/竖向椭圆（1:1.10），
  删除泪滴形与拖尾——单颗不得再像流星；雨幕有效 α 压到 0.02。以下批 10 要求保留：
  - 形状：竖向椭圆（宽:高 = 1 : 1.10），无泪滴、无拖尾；轮廓 blur 1.2px。
  - 尺寸：70% 2–5px / 25% 5–9px / 5% 9–14px（不再均匀）。
  - 内部：α 0.03–0.08 近乎透明 + 轻微高斯模糊（折射感），禁止通体白填充。
  - 高光：只留一个偏心小点（直径 1–2px、偏左上 25–35%、白 α 0.55–0.85、blur 0.6）。
  - 边缘：1px 折射环 α 0.10–0.18（白/深蓝二选一），删除上缘白弧与下缘暗边。
  - 5% 大水珠额外加下缘内侧 1px 白弧 α 0.20–0.25。
  - 密度：512² 共约 260 颗，上半区加密 1.3×；3×3 平铺拷贝保证无缝。
  - 雨幕细竖线保留但压到有效 α 0.02（贴图内按 1/QML 层 opacity 预乘）。

R107 CPU 结构保持：远层/雨幕已并入同一张 water-drops.png，QML 只有 1 个全屏 Image。
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

SS = 2                 # 超采样
SIZE = 512             # 贴图边长
TOTAL = 260            # 水珠总数（批 10）
UPPER_RATIO = 1.3      # 上半区密度倍数
LAYER_OPACITY = 0.55   # QML 水珠层基准 opacity（雨幕 α 预乘用）
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def _size_bucket(rnd, bucket):
    """按 70% / 25% / 5% 三档给出直径。"""
    if bucket == 0:
        return 2.0 + rnd.random() * 3.0      # 2–5
    if bucket == 1:
        return 5.0 + rnd.random() * 4.0      # 5–9
    return 9.0 + rnd.random() * 5.0          # 9–14


def _shape_points(cx, cy, rx, ry):
    """竖向椭圆（1:1.10），无泪滴收窄——批 11：单颗不得有方向性。"""
    return [(cx + rx * math.cos(2.0 * math.pi * i / 40.0),
             cy + ry * math.sin(2.0 * math.pi * i / 40.0)) for i in range(40)]


def _draw_drop(body, accents, rnd, x, y, dia):
    """画一颗扁平透镜水珠（body: 内部+折射环；accents: 偏心高光+可选下缘白弧）。"""
    bd, ad = ImageDraw.Draw(body), ImageDraw.Draw(accents)
    rx = dia * 0.5 * SS
    ry = rx * 1.10                                  # 宽:高 = 1 : 1.10（批 11 固定）
    for ox in (-SIZE, 0, SIZE):
        for oy in (-SIZE, 0, SIZE):
            cx = x * SS + ox * SS
            cy = y * SS + oy * SS
            pts = _shape_points(cx, cy, rx, ry)

            # 内部：几乎透明，能透出天空/云
            inner_a = int(255 * (0.03 + rnd.random() * 0.05))
            bd.polygon(pts, fill=(205, 226, 255, inner_a))

            # 边缘：1px 折射环，白 / 深蓝二选一
            ring_a = int(255 * (0.10 + rnd.random() * 0.08))
            if rnd.random() < 0.7:
                ring_color = (255, 255, 255, ring_a)
            else:
                ring_color = (10, 30, 72, ring_a)
            bd.line(pts + [pts[0]], fill=ring_color,
                    width=max(1, SS), joint="curve")

            # 偏心单点高光（圆心偏左上 25%–35%）
            hr = (0.5 + rnd.random() * 0.5) * SS      # 直径 1–2px
            hx = cx - rx * (0.25 + rnd.random() * 0.10)
            hy = cy - ry * (0.25 + rnd.random() * 0.10)
            hi_a = int(255 * (0.55 + rnd.random() * 0.30))
            ad.ellipse([hx - hr, hy - hr, hx + hr, hy + hr],
                       fill=(255, 255, 255, hi_a))

            # 5% 大水珠：下缘内侧 1px 白弧（透镜边缘折光）
            if dia >= 5.0 and rnd.random() < 0.05:
                ax, ay = rx * 0.82, ry * 0.82
                angle_a = int(255 * (0.20 + rnd.random() * 0.05))
                ad.arc([cx - ax, cy - ay, cx + ax, cy + ay], 25, 155,
                       fill=(255, 255, 255, angle_a), width=max(1, SS))


def _make_veil():
    """极淡纯竖直雨幕；α 按 1/LAYER_OPACITY 预乘，上屏有效 0.02（批 11）。"""
    rnd = random.Random(20261026)
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for _ in range(65):
        x = rnd.random() * SIZE
        w = 0.5 + rnd.random() * 0.8
        h = 15 + rnd.random() * 60
        y = rnd.random() * SIZE
        a = int(255 * 0.02 / LAYER_OPACITY)
        for oy in (-SIZE, 0, SIZE):
            for ox in (-SIZE, 0, SIZE):
                d.line([(x + ox, y + oy), (x + ox, y + oy + h)],
                       fill=(207, 224, 255, a), width=1)
    return img.filter(ImageFilter.GaussianBlur(0.4))


def main():
    rnd = random.Random(20261026)
    big = SIZE * SS
    body = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    accents = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    # 上半区 : 下半区 = 1.3 : 1（总数 260 → 147 / 113）
    n_upper = int(round(TOTAL * UPPER_RATIO / (1.0 + UPPER_RATIO)))
    halves = [(n_upper, 0.0, SIZE * 0.5), (TOTAL - n_upper, SIZE * 0.5, SIZE)]

    buckets = [0, 0, 0]
    made = 0
    for n_half, y0, y1 in halves:
        # 每个半区内部同样保持 70% / 25% / 5%
        b0 = round(n_half * 0.70)
        b1 = round(n_half * 0.25)
        local = (b0, b1, n_half - b0 - b1)
        for bucket, n in enumerate(local):
            buckets[bucket] += n
            for _ in range(n):
                x = rnd.random() * SIZE
                y = y0 + rnd.random() * (y1 - y0)
                dia = _size_bucket(rnd, bucket)
                _draw_drop(body, accents, rnd, x, y, dia)
                made += 1

    # 轮廓 blur 1.2px（SS 降采样已提供约 0.5px 抗锯齿，这里再补 0.8px 达到总柔化 ≈1.2px）
    body = body.resize((SIZE, SIZE), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.8))
    accents = accents.resize((SIZE, SIZE), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6))

    drops = Image.alpha_composite(body, accents)
    merged = Image.alpha_composite(_make_veil(), drops)
    merged.save(os.path.join(OUT, "water-drops.png"))

    # 统计：水珠实际总数（排除雨幕线后的连通点无法简单计数，这里按生成参数报）
    print("generated water-drops.png: %dx%d drops=%d (upper=%d lower=%d, %.2f:1)"
          % (SIZE, SIZE, made, n_upper, TOTAL - n_upper, UPPER_RATIO))
    print("size buckets: 2-5px=%d 5-9px=%d 9-14px=%d; teardrop=0%%; "
          "bottom-inner-arc=5%% of >=5px drops" % tuple(buckets))
    print("veil effective alpha=0.02 (in-texture %.4f)" % (0.02 / LAYER_OPACITY))


if __name__ == "__main__":
    main()
