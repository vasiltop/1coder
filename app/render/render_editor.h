#pragma once

#include "editor/editor.h"
#include "render/draw.h"
#include "render/theme.h"

// Paints the editor. Reads editor state and never writes to it, so "does
// editing work" and "does drawing work" stay independently answerable.

struct RenderContext {
  DrawList *draw;
  GlyphAtlas *atlas;
  Theme theme;

  // Cell dimensions in pixels, derived from the font and rounded to whole
  // pixels so rows land on exact boundaries.
  f32 cell_width;
  f32 cell_height;

  // Pixel y where the panel area ends and the command line begins. Recomputed
  // each frame, since it depends on the window height.
  f32 panel_bottom;
};

void RenderContextInit(RenderContext *ctx, DrawList *draw, GlyphAtlas *atlas);

// Cells the window holds at the current size, which is what the editor lays
// panels out in.
[[nodiscard]] RectS32 RenderScreenCells(const RenderContext *ctx, i32 pixel_width,
                                        i32 pixel_height);

void RenderEditor(RenderContext *ctx, Editor *ed, i32 pixel_width, i32 pixel_height);
