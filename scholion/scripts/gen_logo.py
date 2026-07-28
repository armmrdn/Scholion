#!/usr/bin/env python3
# Regenerate include/logo_data.h from resources/scholion_icon_1024.png.
# Usage: python3 scripts/gen_logo.py   (run from the scholion/ directory)
from PIL import Image
SIZE = 128
im = Image.open("resources/scholion_icon_1024.png").convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
data = im.tobytes()
out = ["#pragma once",
       "// Auto-generated from resources/scholion_icon_1024.png (downscaled to %dx%d RGBA)." % (SIZE,SIZE),
       "// Regenerate with scripts/gen_logo.py if the icon changes. Do not hand-edit.",
       "", "static const int LOGO_W = %d;" % SIZE, "static const int LOGO_H = %d;" % SIZE,
       "static const unsigned char LOGO_RGBA[%d] = {" % len(data)]
line = "  "
for b in data:
    line += "%d," % b
    if len(line) >= 110: out.append(line); line = "  "
if line.strip(): out.append(line)
out.append("};")
open("include/logo_data.h","w").write("\n".join(out) + "\n")
print("wrote include/logo_data.h")
