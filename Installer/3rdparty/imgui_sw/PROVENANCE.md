The two source files were copied from the local `imgui_software_renderer`
revision `c590d389a798a88b7564e2c6ea87685cf2d50f09`. That revision contains
FastSampler's compatibility changes and was never pushed to the upstream
repository, so a GitHub runner cannot fetch it as a submodule.

Changed here since the copy (2026-10-16), both in `imgui_sw.cpp`:

- `ColorInt::toUint32()` packs with ImGui's `IM_COL32_*_SHIFT`. It used a
  fixed RGBA order, so with `IMGUI_USE_BGRA_PACKED_COLOR` every blended pixel
  (anti-aliased glyph edges, icons) had red and blue swapped against the
  pixels written directly.
- `paint_uniform_textured_rectangle` copies the vertex colour only when it is
  opaque. Filled rectangles reach it through the white texel, so a
  translucent one (the modal dim, `CODE_COLOR_ORANGE_TRANSLUCENT`) was drawn
  opaque.

`src/tests/imgui_sw_tests.cpp` covers both.

Changed in the Sonic Reimplants installer's copy only (2026-10-17): `paint_draw_list`
reads each command's `VtxOffset` and `IdxOffset` instead of walking the buffers in
order. ImGui moves a modal's dimmed background to the front of the command list
while its vertices stay at the end, so the dim was painted over the modal.

Upstream: https://github.com/emilk/imgui_software_renderer
Author: Emil Ernerfeldt. The original license is retained at the top of each file.
The old local clone is ignored and remains available for reference.
