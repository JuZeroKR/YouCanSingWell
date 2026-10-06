"""assets/youcansingwell.ico 만들기 (외부 라이브러리 없이). 둥근 배경 위에 음표 모양.
   python scripts/make_icon.py
"""
import math
import os
import struct

SIZES = [16, 32, 48, 256]


def render(n):
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]
    c = (n - 1) / 2
    r = n * 0.47
    for y in range(n):
        for x in range(n):
            d = math.hypot(x - c, y - c)
            if d <= r:
                # 배경: 위 보라 → 아래 파랑 그라데이션, 가장자리 안티에일리어싱
                t = y / (n - 1)
                col = (int(120 - 40 * t), int(90 + 60 * t), int(220 + 20 * t))
                a = 255 if d <= r - 1 else int(255 * (r - d))
                px[y][x] = (col[0], col[1], col[2], max(0, min(255, a)))
    # 음표: 머리(타원) + 기둥 + 깃발. 흰색
    hx, hy = c - n * 0.10, c + n * 0.22
    hw, hh = n * 0.17, n * 0.12
    stem_x = hx + hw * 0.85
    for y in range(n):
        for x in range(n):
            if px[y][x][3] == 0:
                continue
            on = False
            # 기울어진 타원 머리
            dx, dy = x - hx, y - hy
            ang = math.radians(-20)
            ex = (dx * math.cos(ang) - dy * math.sin(ang)) / hw
            ey = (dx * math.sin(ang) + dy * math.cos(ang)) / hh
            if ex * ex + ey * ey <= 1:
                on = True
            # 기둥
            if stem_x - n * 0.035 <= x <= stem_x + n * 0.035 and c - n * 0.33 <= y <= hy:
                on = True
            # 깃발 (기둥 위에서 오른쪽 아래로 흐르는 곡선 띠)
            fy = y - (c - n * 0.33)
            if 0 <= fy <= n * 0.28:
                u = fy / (n * 0.28)
                fx = stem_x + n * (0.30 * math.sin(u * math.pi * 0.9)) + n * 0.03
                if fx - n * 0.07 <= x <= fx + n * 0.02 and x >= stem_x:
                    on = True
            if on:
                px[y][x] = (255, 255, 255, px[y][x][3])
    return px


def bmp_dib(px, n):
    # BITMAPINFOHEADER + BGRA 상향 행 + AND 마스크 (전부 0)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, n * n * 4, 0, 0, 0, 0)
    body = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = px[y][x]
            body += bytes((b, g, r, a))
    mask_row = ((n + 31) // 32) * 4
    body += bytes(mask_row * n)
    return hdr + bytes(body)


def main():
    images = [(n, bmp_dib(render(n), n)) for n in SIZES]
    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for n, data in images:
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    os.makedirs('assets', exist_ok=True)
    with open('assets/youcansingwell.ico', 'wb') as f:
        f.write(out)
    print('wrote assets/youcansingwell.ico', len(out), 'bytes')


if __name__ == '__main__':
    main()
