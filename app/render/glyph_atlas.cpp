#include "render/glyph_atlas.h"
#include "render/render_metrics.h"

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

// Compares a font's name-table entry against ASCII. Microsoft-platform names
// are UTF-16BE; Mac-platform ones are single-byte.
[[nodiscard]] bool NameEquals(const char *name, i32 length, bool utf16, String8 wanted) {
  u64 count = utf16 ? (u64)length / 2 : (u64)length;
  if (count != wanted.size) return false;

  for (u64 i = 0; i < count; i += 1) {
    u8 c = utf16 ? (u8)name[i * 2 + 1] : (u8)name[i];
    if (c != wanted.str[i]) return false;
  }
  return true;
}

[[nodiscard]] bool FaceNameMatches(const stbtt_fontinfo *info, i32 name_id, String8 wanted) {
  i32 length = 0;
  const char *name = stbtt_GetFontNameString(info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                             STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH,
                                             name_id);
  if (name && NameEquals(name, length, true, wanted)) return true;

  name = stbtt_GetFontNameString(info, &length, STBTT_PLATFORM_ID_MAC, 0, 0, name_id);
  return name && NameEquals(name, length, false, wanted);
}

// Finds the face whose family name is exactly `family` and whose style is
// "Regular".
//
// stbtt_FindMatchingFont is not usable here: it matches loosely, so asking a
// 162-face Iosevka collection for "Iosevka Fixed" hands back "Iosevka Fixed
// Thin" -- a real face, just the wrong weight, and one whose thinness is
// subtle enough to look like a rendering problem rather than a wrong font.
[[nodiscard]] i32 FindFaceOffset(const u8 *font_data, String8 family) {
  i32 count = stbtt_GetNumberOfFonts(font_data);
  if (count <= 0) return -1;

  for (i32 i = 0; i < count; i += 1) {
    i32 offset = stbtt_GetFontOffsetForIndex(font_data, i);
    if (offset < 0) continue;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, font_data, offset)) continue;

    if (FaceNameMatches(&info, 1, family) && FaceNameMatches(&info, 2, Str8Lit("Regular"))) {
      return offset;
    }
  }
  return -1;
}

}  // namespace

bool GlyphAtlasInit(GlyphAtlas *atlas, Arena *arena, SDL_Renderer *renderer, String8 font_path,
                    f32 pixel_height, String8 face_name) {
  *atlas = GlyphAtlas{};
  atlas->arena = arena;
  atlas->pixel_height = pixel_height;

  atlas->font_file = OsFileMap(font_path);
  if (!atlas->font_file.ok) return false;
  u8 *font_data = atlas->font_file.data;

  stbtt_fontinfo *info = PushStruct(arena, stbtt_fontinfo);
  atlas->font_info = info;

  // In a collection, find the requested face by exact name; fall back to the
  // first one, which is all a plain .ttf has anyway.
  i32 offset = (face_name.size > 0) ? FindFaceOffset(font_data, face_name) : -1;
  if (offset < 0) offset = stbtt_GetFontOffsetForIndex(font_data, 0);
  if (offset < 0 || !stbtt_InitFont(info, font_data, offset)) return false;

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
  atlas->advance = RenderCellMetric((f32)advance * atlas->scale);

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
  OsFileUnmap(&atlas->font_file);
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

String8 GlyphAtlasFindMonospaceFont(Arena *arena, String8 *out_face) {
  struct Candidate {
    const char *path;
    const char *face;  // which face to ask for, when the file is a collection
  };

  // Ordered by preference. Iosevka first, since that is what the user's i3
  // config asks for; "Iosevka Fixed" is the strictly monospaced cut of it,
  // which is what a cell grid wants.
  const Candidate candidates[] = {
      {"/usr/share/fonts/TTF/Iosevka.ttc", "Iosevka Fixed"},
      {"/usr/share/fonts/TTF/Iosevka-Regular.ttf", ""},
      {"/usr/share/fonts/truetype/iosevka/Iosevka-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/IosevkaTerm-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf", ""},
      {"/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/FiraCode-Regular.ttf", ""},
      {"/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/DejaVuSansMono.ttf", ""},
      {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", ""},
      {"/usr/share/fonts/TTF/liberation/LiberationMono-Regular.ttf", ""},
      {"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", ""},
      {"/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/Hack-Regular.ttf", ""},
      {"/usr/share/fonts/TTF/UbuntuMono-R.ttf", ""},
      {"/System/Library/Fonts/Menlo.ttc", "Menlo Regular"},
      {"/System/Library/Fonts/SFNSMono.ttf", ""},
      {"C:\\Windows\\Fonts\\consola.ttf", ""},
  };

  for (u64 i = 0; i < ArrayCount(candidates); i += 1) {
    String8 path = Str8C(candidates[i].path);
    if (!OsFileExists(path)) continue;
    if (out_face) *out_face = PushStr8Copy(arena, Str8C(candidates[i].face));
    return PushStr8Copy(arena, path);
  }

  if (out_face) *out_face = String8{nullptr, 0};
  return String8{nullptr, 0};
}
