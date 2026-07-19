#include "render/glyph_atlas.h"

#include "os/os_file.h"

#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace {

constexpr i32 kAtlasWidth = 1024;
constexpr i32 kAtlasHeight = 1024;
constexpr u64 kGlyphCapacity = 2048;  // power of two, for the mask below
constexpr i32 kGlyphPadding = 1;      // keeps bilinear sampling off neighbours

[[nodiscard]] u64 HashCodepoint(u32 codepoint) {
  u64 hash = codepoint * 2654435761ULL;
  return hash ^ (hash >> 29);
}

// Uploads a coverage bitmap as white pixels with the coverage as alpha, so
// vertex colours can tint the glyph at draw time.
void UploadCoverage(SDL_Texture *texture, Arena *scratch, i32 x, i32 y, i32 w, i32 h,
                    const u8 *coverage) {
  if (w <= 0 || h <= 0) return;

  u32 *pixels = PushArrayNoZero(scratch, u32, (u64)w * (u64)h);
  for (i32 i = 0; i < w * h; i += 1) {
    pixels[i] = ((u32)coverage[i] << 24) | 0x00FFFFFFu;
  }

  SDL_Rect rect = {x, y, w, h};
  SDL_UpdateTexture(texture, &rect, pixels, w * (i32)sizeof(u32));
}

}  // namespace

bool GlyphAtlasInit(GlyphAtlas *atlas, Arena *arena, SDL_Renderer *renderer, String8 font_path,
                    f32 pixel_height) {
  *atlas = GlyphAtlas{};
  atlas->arena = arena;
  atlas->pixel_height = pixel_height;

  FileContents font = OsFileRead(arena, font_path);
  if (!font.ok || font.data.size == 0) return false;
  atlas->font_data = font.data.str;

  stbtt_fontinfo *info = PushStruct(arena, stbtt_fontinfo);
  atlas->font_info = info;

  i32 offset = stbtt_GetFontOffsetForIndex(atlas->font_data, 0);
  if (offset < 0 || !stbtt_InitFont(info, atlas->font_data, offset)) return false;

  atlas->scale = stbtt_ScaleForPixelHeight(info, pixel_height);

  i32 ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
  atlas->ascent = (f32)ascent * atlas->scale;
  atlas->descent = (f32)descent * atlas->scale;
  atlas->line_height = (f32)(ascent - descent + line_gap) * atlas->scale;

  // The face is assumed monospace, so one representative glyph sets the cell
  // width for the whole grid.
  i32 advance = 0, bearing = 0;
  stbtt_GetCodepointHMetrics(info, 'M', &advance, &bearing);
  atlas->advance = (f32)advance * atlas->scale;

  atlas->width = kAtlasWidth;
  atlas->height = kAtlasHeight;
  atlas->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, atlas->width, atlas->height);
  if (!atlas->texture) return false;

  SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
  // Nearest keeps the grid crisp; the atlas is sampled at 1:1 anyway.
  SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_NEAREST);

  atlas->glyphs = PushArray(arena, Glyph, kGlyphCapacity);
  atlas->capacity = kGlyphCapacity;

  // Reserve (0,0) as opaque white so solid rectangles can share this texture,
  // which is what keeps a frame down to a single draw call.
  {
    TempArena scratch = ScratchBegin1(arena);
    u8 white = 255;
    UploadCoverage(atlas->texture, scratch.arena, 0, 0, 1, 1, &white);
    ScratchEnd(scratch);
  }
  atlas->shelf_x = 2;
  atlas->shelf_y = 0;
  atlas->shelf_height = 2;

  atlas->valid = true;
  return true;
}

void GlyphAtlasDestroy(GlyphAtlas *atlas) {
  if (atlas->texture) SDL_DestroyTexture(atlas->texture);
  atlas->texture = nullptr;
  atlas->valid = false;
}

RectF32 GlyphAtlasWhiteUV(const GlyphAtlas *atlas) {
  // The centre of the reserved pixel, so no neighbouring texel can bleed in.
  f32 u = 0.5f / (f32)atlas->width;
  f32 v = 0.5f / (f32)atlas->height;
  return RectF32{u, v, u, v};
}

const Glyph *GlyphAtlasGet(GlyphAtlas *atlas, u32 codepoint) {
  u64 mask = atlas->capacity - 1;
  u64 slot = HashCodepoint(codepoint) & mask;

  while (atlas->glyphs[slot].occupied) {
    if (atlas->glyphs[slot].codepoint == codepoint) return &atlas->glyphs[slot];
    slot = (slot + 1) & mask;
  }

  Glyph *glyph = &atlas->glyphs[slot];
  glyph->occupied = true;
  glyph->codepoint = codepoint;
  glyph->advance = atlas->advance;

  stbtt_fontinfo *info = (stbtt_fontinfo *)atlas->font_info;

  i32 advance = 0, bearing = 0;
  stbtt_GetCodepointHMetrics(info, (i32)codepoint, &advance, &bearing);
  // Keep the monospace cell width even for glyphs the face draws differently,
  // so the column grid never drifts.
  glyph->advance = atlas->advance;

  i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetCodepointBitmapBox(info, (i32)codepoint, atlas->scale, atlas->scale, &x0, &y0, &x1,
                              &y1);

  i32 w = x1 - x0;
  i32 h = y1 - y0;

  // Whitespace and unmapped codepoints have no bitmap; they still occupy a
  // cell, so return a valid but empty glyph.
  if (w <= 0 || h <= 0) {
    glyph->uv = GlyphAtlasWhiteUV(atlas);
    glyph->width = 0;
    glyph->height = 0;
    return glyph;
  }

  // Advance the shelf, wrapping to a new row when this one is full.
  if (atlas->shelf_x + w + kGlyphPadding > atlas->width) {
    atlas->shelf_x = 0;
    atlas->shelf_y += atlas->shelf_height + kGlyphPadding;
    atlas->shelf_height = 0;
  }
  if (atlas->shelf_y + h > atlas->height) {
    // The atlas is full. Returning an empty glyph degrades to missing text
    // rather than corrupting what is already cached.
    glyph->uv = GlyphAtlasWhiteUV(atlas);
    glyph->width = 0;
    glyph->height = 0;
    return glyph;
  }

  TempArena scratch = ScratchBegin1(atlas->arena);
  u8 *coverage = PushArrayNoZero(scratch.arena, u8, (u64)w * (u64)h);
  stbtt_MakeCodepointBitmap(info, coverage, w, h, w, atlas->scale, atlas->scale,
                            (i32)codepoint);
  UploadCoverage(atlas->texture, scratch.arena, atlas->shelf_x, atlas->shelf_y, w, h, coverage);
  ScratchEnd(scratch);

  glyph->uv = RectF32{
      (f32)atlas->shelf_x / (f32)atlas->width,
      (f32)atlas->shelf_y / (f32)atlas->height,
      (f32)(atlas->shelf_x + w) / (f32)atlas->width,
      (f32)(atlas->shelf_y + h) / (f32)atlas->height,
  };
  glyph->width = (f32)w;
  glyph->height = (f32)h;
  glyph->bearing_x = (f32)x0;
  glyph->bearing_y = (f32)y0;

  atlas->shelf_x += w + kGlyphPadding;
  atlas->shelf_height = Max(atlas->shelf_height, h);

  return glyph;
}

String8 GlyphAtlasFindMonospaceFont(Arena *arena) {
  // Ordered by preference. Editors live or die on their font, so the ones with
  // proper programming-face metrics come first.
  const char *candidates[] = {
      "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
      "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
      "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
      "/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/Hack-Regular.ttf",
      "/usr/share/fonts/TTF/UbuntuMono-R.ttf",
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/SFNSMono.ttf",
      "C:\\Windows\\Fonts\\consola.ttf",
  };

  for (u64 i = 0; i < ArrayCount(candidates); i += 1) {
    String8 path = Str8C(candidates[i]);
    if (OsFileExists(path)) return PushStr8Copy(arena, path);
  }

  return String8{nullptr, 0};
}
