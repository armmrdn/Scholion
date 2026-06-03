#!/usr/bin/env python3
"""
Generate Scholion.app icon → resources/AppIcon.icns
Requires: pip3 install Pillow
"""
import shutil, subprocess, sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("Pillow not found — run: pip3 install Pillow")

RESOURCES = Path(__file__).parent.parent / "resources"
ICONSET   = RESOURCES / "AppIcon.iconset"
ICNS      = RESOURCES / "AppIcon.icns"

ICON_SIZES = [
    ("icon_16x16.png",        16),
    ("icon_16x16@2x.png",     32),
    ("icon_32x32.png",        32),
    ("icon_32x32@2x.png",     64),
    ("icon_128x128.png",     128),
    ("icon_128x128@2x.png",  256),
    ("icon_256x256.png",     256),
    ("icon_256x256@2x.png",  512),
    ("icon_512x512.png",     512),
    ("icon_512x512@2x.png", 1024),
]


def rounded_rect(draw, box, radius, fill):
    x0, y0, x1, y1 = box
    r = max(1, radius)
    draw.ellipse([x0,       y0,       x0+2*r, y0+2*r], fill=fill)
    draw.ellipse([x1-2*r,   y0,       x1,     y0+2*r], fill=fill)
    draw.ellipse([x0,       y1-2*r,   x0+2*r, y1    ], fill=fill)
    draw.ellipse([x1-2*r,   y1-2*r,   x1,     y1    ], fill=fill)
    draw.rectangle([x0+r, y0,   x1-r, y1   ], fill=fill)
    draw.rectangle([x0,   y0+r, x1,   y1-r ], fill=fill)


def draw_shadow(img, box, radius, blur=18, alpha=60):
    """Draw a soft drop shadow by rendering a shape onto a separate layer and blurring."""
    shadow_layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow_layer, "RGBA")
    rounded_rect(sd, box, radius, (0, 0, 0, alpha))
    shadow_layer = shadow_layer.filter(ImageFilter.GaussianBlur(radius=blur))
    img.alpha_composite(shadow_layer)


def draw_icon(sz):
    img = Image.new("RGBA", (sz, sz), (0, 0, 0, 0))
    d   = ImageDraw.Draw(img, "RGBA")
    s   = sz / 1024.0

    # --- Background (near-black rounded square) ---
    rounded_rect(d, [0, 0, sz - 1, sz - 1], int(150 * s), (14, 14, 14, 255))

    # --- Stack geometry ---
    pw  = int(456 * s)   # paper width
    ph  = int(612 * s)   # paper height  (≈ letter aspect ratio)
    off = int(50  * s)   # per-sheet offset (toward lower-right)
    pr  = int(18  * s)   # paper corner radius

    # Center the entire stack footprint in the canvas
    total_w = pw + 2 * off
    total_h = ph + 2 * off
    x0 = (sz - total_w) // 2
    y0 = (sz - total_h) // 2

    # Sheet definitions: drawn back → front
    sheets = [
        # (x-origin, y-origin, fill-color)
        (x0 + 2*off, y0 + 2*off, (58,  58,  58,  255)),   # back  — dark gray
        (x0 +   off, y0 +   off, (118, 118, 118, 255)),   # mid   — medium gray
        (x0,         y0,         (238, 234, 226, 255)),   # front — warm white
    ]

    for sx, sy, color in sheets:
        # Soft drop shadow for each sheet
        blur_r = max(2, int(18 * s))
        draw_shadow(img, [sx + int(10*s), sy + int(12*s),
                          sx + pw + int(10*s), sy + ph + int(12*s)],
                    pr, blur=blur_r, alpha=55)
        # Sheet body
        d = ImageDraw.Draw(img, "RGBA")
        rounded_rect(d, [sx, sy, sx + pw, sy + ph], pr, color)

    # --- Subtle text lines on the front sheet only ---
    fx, fy    = x0, y0
    lx0       = fx + int(60 * s)
    lx1       = fx + pw - int(60 * s)
    ly        = fy + int(108 * s)
    lh        = max(1, int(11 * s))
    lsp       = int(76 * s)
    line_color = (210, 205, 196, 255)
    line_widths = [1.0, 1.0, 1.0, 1.0, 0.58]   # last line shorter (paragraph end)
    for frac in line_widths:
        d.rectangle([lx0, ly, lx0 + int((lx1 - lx0) * frac), ly + lh], fill=line_color)
        ly += lsp

    return img


def main():
    shutil.rmtree(ICONSET, ignore_errors=True)
    ICONSET.mkdir(parents=True, exist_ok=True)

    print("Rendering 1024×1024 master icon…")
    master = draw_icon(1024)
    master.save(RESOURCES / "scholion_icon_1024.png")
    print(f"  Saved preview → {RESOURCES / 'scholion_icon_1024.png'}")

    print("Generating iconset…")
    for name, sz in ICON_SIZES:
        resized = master.resize((sz, sz), Image.LANCZOS)
        resized.save(ICONSET / name)
        print(f"  {name}")

    print("Running iconutil…")
    result = subprocess.run(
        ["iconutil", "-c", "icns", str(ICONSET), "-o", str(ICNS)],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        sys.exit(f"iconutil failed:\n{result.stderr}")

    shutil.rmtree(ICONSET)
    print(f"Done → {ICNS}")


if __name__ == "__main__":
    main()
