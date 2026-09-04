# Genera include/web_assets.h: UI gzippata, icona, e splash screen iOS
# (apple-touch-startup-image) disegnate proceduralmente per le risoluzioni iPhone.
# Tutto in PROGMEM: l'OTA del firmware aggiorna anche gli asset.
import gzip
import hashlib
import math
import os
import struct
import zlib

Import("env")  # noqa: F821

SRC = os.path.join(env["PROJECT_DIR"], "data-src", "index.html")  # noqa: F821
ICON = os.path.join(env["PROJECT_DIR"], "data-src", "icon.png")  # noqa: F821
OUT = os.path.join(env["PROJECT_DIR"], "include", "web_assets.h")  # noqa: F821

# Risoluzioni iPhone (portrait, pixel fisici) per apple-touch-startup-image.
SPLASH_SIZES = [
    (750, 1334),   # SE2/SE3, 8 (@2x 375x667)
    (828, 1792),   # XR, 11 (@2x 414x896)
    (1125, 2436),  # X/XS/11 Pro/12-13 mini (@3x 375x812)
    (1170, 2532),  # 12/13/14 (@3x 390x844)
    (1179, 2556),  # 14 Pro/15/16 (@3x 393x852)
    (1206, 2622),  # 16 Pro (@3x 402x874)
    (1242, 2688),  # XS Max/11 Pro Max (@3x 414x896)
    (1284, 2778),  # 12/13 Pro Max, 14 Plus (@3x 428x926)
    (1290, 2796),  # 14 Pro Max/15-16 Plus/15 Pro Max (@3x 430x932)
    (1320, 2868),  # 16 Pro Max (@3x 440x956)
]

BG = bytes((42, 120, 214))     # blu della UI
WHITE = bytes((255, 255, 255))
RED = bytes((227, 73, 72))

FONT = {  # 5x7
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "x": ["00000", "00000", "10001", "01010", "00100", "01010", "10001"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "h": ["10000", "10000", "10110", "11001", "10001", "10001", "10001"],
    "e": ["00000", "00000", "01110", "10001", "11111", "10000", "01110"],
    "r": ["00000", "00000", "10110", "11001", "10000", "10000", "10000"],
    "m": ["00000", "00000", "11010", "10101", "10101", "10101", "10101"],
}
TEXT = "AxHTherm"


def png_chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def make_splash(W, H):
    cx = W // 2
    stem_w = max(24, int(W * 0.052))
    bulb_r = max(40, int(W * 0.085))
    bulb_cy = int(H * 0.44)
    stem_top = int(H * 0.30)
    red_top = int(H * 0.345)
    red_w = int(stem_w * 0.5)
    red_r = int(bulb_r * 0.72)
    scale = max(4, W // 62)
    text_w = (len(TEXT) * 6 - 1) * scale
    text_x = cx - text_w // 2
    text_y = bulb_cy + bulb_r + int(H * 0.045)

    bg_row = BG * W
    rows = []
    for y in range(H):
        row = None

        def paint(x0, x1, color):
            nonlocal row
            if row is None:
                row = bytearray(bg_row)
            x0 = max(0, x0)
            x1 = min(W, x1)
            if x1 > x0:
                row[x0 * 3:x1 * 3] = color * (x1 - x0)

        if stem_top <= y < bulb_cy:
            paint(cx - stem_w // 2, cx + stem_w // 2, WHITE)
        d = y - bulb_cy
        if abs(d) <= bulb_r:
            half = int(math.sqrt(bulb_r * bulb_r - d * d))
            paint(cx - half, cx + half, WHITE)
        if red_top <= y < bulb_cy:
            paint(cx - red_w // 2, cx + red_w // 2, RED)
        if abs(d) <= red_r:
            half = int(math.sqrt(red_r * red_r - d * d))
            paint(cx - half, cx + half, RED)
        g = (y - text_y) // scale
        if 0 <= g < 7:
            for ci, ch in enumerate(TEXT):
                bits = FONT[ch][g]
                gx = text_x + ci * 6 * scale
                for col in range(5):
                    if bits[col] == "1":
                        paint(gx + col * scale, gx + (col + 1) * scale, WHITE)

        rows.append(b"\x00" + (bytes(row) if row is not None else bg_row))

    raw = b"".join(rows)
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(raw, 9))
            + png_chunk(b"IEND", b""))


def carr(data):
    return ", ".join(str(b) for b in data)


def generate():
    if not os.path.exists(SRC):
        # FALLIRE il build, non saltare in silenzio: saltare lascerebbe include/web_assets.h
        # stantio (UI vecchia spedita col firmware nuovo) o assente (che non compilerebbe),
        # senza alcun segnale che l'interfaccia non è stata rigenerata.
        raise Exception("gzip_assets: ERRORE — data-src/index.html mancante (%s): "
                        "impossibile rigenerare la UI. Build interrotto." % SRC)
    with open(SRC, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    etag = hashlib.md5(gz).hexdigest()[:16]

    parts = []
    parts.append("// Auto-generato da tools/gzip_assets.py — NON modificare a mano.\n")
    parts.append("#pragma once\n#include <pgmspace.h>\n")
    parts.append("static const uint8_t WEB_INDEX_GZ[] PROGMEM = {%s};\n" % carr(gz))
    parts.append("static const unsigned int WEB_INDEX_GZ_LEN = %d;\n" % len(gz))
    parts.append('static const char WEB_INDEX_ETAG[] = "\\"%s\\"";\n' % etag)
    if os.path.exists(ICON):
        with open(ICON, "rb") as fi:
            png = fi.read()
        parts.append("static const uint8_t WEB_ICON_PNG[] PROGMEM = {%s};\n" % carr(png))
        parts.append("static const unsigned int WEB_ICON_PNG_LEN = %d;\n" % len(png))

    splash_total = 0
    table = []
    for i, (w, h) in enumerate(SPLASH_SIZES):
        png = make_splash(w, h)
        splash_total += len(png)
        parts.append("static const uint8_t SPLASH_%d[] PROGMEM = {%s};\n" % (i, carr(png)))
        table.append('  {"/splash_%dx%d.png", SPLASH_%d, %d},' % (w, h, i, len(png)))
    parts.append("struct WebSplash { const char* path; const uint8_t* data; unsigned int len; };\n")
    parts.append("static const WebSplash WEB_SPLASHES[] = {\n%s\n};\n" % "\n".join(table))
    parts.append("static const unsigned int WEB_SPLASHES_COUNT = %d;\n" % len(SPLASH_SIZES))

    content = "".join(parts)
    if os.path.exists(OUT):
        with open(OUT) as f:
            if f.read() == content:
                return  # invariato: niente riscrittura, niente ricompilazioni inutili
    with open(OUT, "w") as f:
        f.write(content)
    print("gzip_assets: index.html %d B -> %d B gzip; icon %s; %d splash (%d B tot)"
          % (len(raw), len(gz), "ok" if os.path.exists(ICON) else "assente",
             len(SPLASH_SIZES), splash_total))


generate()
