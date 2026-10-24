#!/usr/bin/env python3
"""批 12｜生成水珠折射偏移图 drop-offset.png（256×256 RGBA）。

编码（用户裁决 2026-10-24 批 12）：
  R = y 偏移（128 = 0，<128 向上，>128 向下）
  G = x 偏移（128 = 0，<128 向左，>128 向右）
  B = 深度（半球：中心 255、边缘 0）
  A = 覆盖率（圆内 1、圆外 0，边缘 1px 软化）

偏移幅度：中心 ≈0，半径 0.8R 处最大（方向朝外，∝ d），到边缘回落到 0。
"""
import math

from PIL import Image

SIZE = 256
CENTER = (SIZE - 1) / 2.0
RADIUS = 118.0
PEAK = 120.0  # 编码偏移幅度（255 单位的 0.47）


def ramp(rho: float) -> float:
    """0→0.8R 线性升到 1，0.8R→1.0R 线性回落到 0。"""
    if rho <= 0.8:
        return rho / 0.8
    return max(0.0, (1.0 - rho) / 0.2)


def main() -> None:
    img = Image.new("RGBA", (SIZE, SIZE), (128, 128, 0, 0))
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            dx = (x - CENTER) / RADIUS
            dy = (y - CENTER) / RADIUS
            rho = math.hypot(dx, dy)
            if rho >= 1.0:
                px[x, y] = (128, 128, 0, 0)
                continue
            if rho < 1e-6:
                ox = oy = 0.0
            else:
                mag = ramp(rho) * PEAK
                ox = dx / rho * mag
                oy = dy / rho * mag
            r = max(0, min(255, int(round(128 + oy))))
            g = max(0, min(255, int(round(128 + ox))))
            depth = math.sqrt(max(0.0, 1.0 - rho * rho))
            a = max(0.0, min(1.0, (1.0 - rho) * RADIUS))  # 1px 软化边
            px[x, y] = (r, g, int(round(255 * depth)), int(round(255 * a)))
    img.save(__file__.rsplit("/", 1)[0] + "/drop-offset.png")
    print("wrote drop-offset.png 256x256")


if __name__ == "__main__":
    main()
