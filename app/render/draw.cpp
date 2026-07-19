#include "render/draw.h"

namespace {

constexpr u64 kMaxVertices = 1 << 17;
constexpr u64 kMaxIndices = kMaxVertices * 3 / 2;

[[nodiscard]] SDL_FColor ToFColor(Vec4F32 c) { return SDL_FColor{c.x, c.y, c.z, c.w}; }

// Appends one quad. Every primitive the editor draws is one of these; solid
// fills simply point at the atlas's reserved white pixel.
void PushQuad(DrawList *draw, RectF32 rect, RectF32 uv, Vec4F32 color) {
  if (draw->vertex_count + 4 > draw->vertex_capacity ||
      draw->index_count + 6 > draw->index_capacity) {
    // Rather than drop geometry, emit what is queued and carry on.
    DrawFlush(draw);
  }
  if (draw->vertex_count + 4 > draw->vertex_capacity) return;

  SDL_FColor fcolor = ToFColor(color);
  u64 base = draw->vertex_count;

  SDL_Vertex *v = &draw->vertices[base];
  v[0] = SDL_Vertex{{rect.x0, rect.y0}, fcolor, {uv.x0, uv.y0}};
  v[1] = SDL_Vertex{{rect.x1, rect.y0}, fcolor, {uv.x1, uv.y0}};
  v[2] = SDL_Vertex{{rect.x1, rect.y1}, fcolor, {uv.x1, uv.y1}};
  v[3] = SDL_Vertex{{rect.x0, rect.y1}, fcolor, {uv.x0, uv.y1}};
  draw->vertex_count += 4;

  i32 *idx = &draw->indices[draw->index_count];
  idx[0] = (i32)base + 0;
  idx[1] = (i32)base + 1;
  idx[2] = (i32)base + 2;
  idx[3] = (i32)base + 0;
  idx[4] = (i32)base + 2;
  idx[5] = (i32)base + 3;
  draw->index_count += 6;
}

}  // namespace

void DrawInit(DrawList *draw, Arena *arena, SDL_Renderer *renderer, GlyphAtlas *atlas) {
  *draw = DrawList{};
  draw->renderer = renderer;
  draw->atlas = atlas;
  draw->vertices = PushArrayNoZero(arena, SDL_Vertex, kMaxVertices);
  draw->vertex_capacity = kMaxVertices;
  draw->indices = PushArrayNoZero(arena, i32, kMaxIndices);
  draw->index_capacity = kMaxIndices;
}

void DrawBegin(DrawList *draw) {
  draw->vertex_count = 0;
  draw->index_count = 0;
  draw->clipping = false;
  SDL_SetRenderClipRect(draw->renderer, nullptr);
}

void DrawFlush(DrawList *draw) {
  if (draw->index_count == 0) {
    draw->vertex_count = 0;
    return;
  }

  SDL_RenderGeometry(draw->renderer, draw->atlas->texture, draw->vertices,
                     (int)draw->vertex_count, draw->indices, (int)draw->index_count);

  draw->vertex_count = 0;
  draw->index_count = 0;
}

void DrawPushClip(DrawList *draw, RectF32 rect) {
  // Queued geometry belongs to the previous clip, so it has to go out first.
  DrawFlush(draw);

  draw->clip = SDL_Rect{(int)rect.x0, (int)rect.y0, (int)(rect.x1 - rect.x0),
                        (int)(rect.y1 - rect.y0)};
  draw->clipping = true;
  SDL_SetRenderClipRect(draw->renderer, &draw->clip);
}

void DrawPopClip(DrawList *draw) {
  DrawFlush(draw);
  draw->clipping = false;
  SDL_SetRenderClipRect(draw->renderer, nullptr);
}

void DrawRect(DrawList *draw, RectF32 rect, Vec4F32 color) {
  if (color.w <= 0.0f) return;
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) return;
  PushQuad(draw, rect, GlyphAtlasWhiteUV(draw->atlas), color);
}

f32 DrawGlyph(DrawList *draw, u32 codepoint, f32 x, f32 baseline_y, Vec4F32 color) {
  const Glyph *glyph = GlyphAtlasGet(draw->atlas, codepoint);

  if (glyph->width > 0.0f && glyph->height > 0.0f && color.w > 0.0f) {
    RectF32 rect = {
        x + glyph->bearing_x,
        baseline_y + glyph->bearing_y,
        x + glyph->bearing_x + glyph->width,
        baseline_y + glyph->bearing_y + glyph->height,
    };
    PushQuad(draw, rect, glyph->uv, color);
  }

  return glyph->advance;
}

f32 DrawText(DrawList *draw, String8 text, f32 x, f32 baseline_y, Vec4F32 color) {
  f32 pen = x;
  for (u64 i = 0; i < text.size;) {
    DecodedCodepoint decoded = Utf8Decode(text, i);
    pen += DrawGlyph(draw, decoded.codepoint, pen, baseline_y, color);
    i += decoded.advance;
  }
  return pen - x;
}
