# Bundled UI font

`include/font_data.h` is DejaVu Sans (v2.37), embedded as an ImGui-compressed byte array.
DejaVu is under the Bitstream Vera / Arev free license (see DejaVu-LICENSE.txt) — redistribution
and bundling are permitted.

## Regenerate
```
curl -sSL -o DejaVuSans.ttf \
  "https://cdn.jsdelivr.net/npm/dejavu-fonts-ttf@2.37.3/ttf/DejaVuSans.ttf"
c++ -O2 -o b2c third_party/imgui/misc/fonts/binary_to_compressed_c.cpp
./b2c DejaVuSans.ttf dejavu_sans > include/font_data.h
```
