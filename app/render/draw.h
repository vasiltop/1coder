#pragma once

#include <SDL3/SDL.h>

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_string.h"
#include "render/glyph_atlas.h"

// Batched 2D drawing.
//
// Everything -- glyphs, fills, the cursor -- is a textured quad against the
// glyph atlas, so a frame accumulates into one vertex buffer and goes out in a
// single SDL_RenderGeometry call. The only thing that forces a flush is a
// change of clip rectangle, which happens once per panel.

struct DrawList {
  SDL_Renderer *renderer;
  GlyphAtlas *atlas;

  SDL_Vertex *vertices;
  u64 vertex_count;
  u64 vertex_capacity;

  i32 *indices;
  u64 index_count;
  u64 index_capacity;

  bool clipping;
  SDL_Rect clip;
};

void DrawInit(DrawList *draw, Arena *arena, SDL_Renderer *renderer, GlyphAtlas *atlas);

void DrawBegin(DrawList *draw);
// Emits everything accumulated so far. Called automatically when the clip
// changes and at the end of a frame.
void DrawFlush(DrawList *draw);

// Restricts subsequent drawing to `rect`, in pixels.
void DrawPushClip(DrawList *draw, RectF32 rect);
void DrawPopClip(DrawList *draw);

void DrawRect(DrawList *draw, RectF32 rect, Vec4F32 color);

// Draws one glyph with its baseline at (x, baseline_y). Returns the pen
// advance, so a caller stepping through text does not have to consult the
// atlas twice.
f32 DrawGlyph(DrawList *draw, u32 codepoint, f32 x, f32 baseline_y, Vec4F32 color);

// Draws UTF-8 text, advancing by the monospace cell width. Returns the width
// drawn.
f32 DrawText(DrawList *draw, String8 text, f32 x, f32 baseline_y, Vec4F32 color);
