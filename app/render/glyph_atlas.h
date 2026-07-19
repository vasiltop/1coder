#pragma once

#include <SDL3/SDL.h>

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "os/os_file.h"

// Glyphs rasterised on demand into one texture.
//
// Every glyph the editor has drawn so far lives in a single atlas, so a whole
// frame -- text, selections, cursors, chrome -- goes out as one piece of
// geometry with one texture bound. Pixel (0,0) is kept opaque white, which lets
// solid rectangles share that texture instead of forcing a second batch.

struct Glyph {
  RectF32 uv;      // texture coordinates, normalised
  f32 advance;     // pen movement
  f32 bearing_x;   // offset from pen to bitmap's left edge
  f32 bearing_y;   // offset from baseline to bitmap's top edge
  f32 width;
  f32 height;
  u32 codepoint;
  bool occupied;
};

struct GlyphAtlas {
  SDL_Texture *texture;
  i32 width;
  i32 height;

  // Shelf packing: glyphs fill a row left to right, then a new row starts below
  // the tallest so far. Cheap, and good enough for one monospace face.
  i32 shelf_x;
  i32 shelf_y;
  i32 shelf_height;

  // Font metrics, in pixels.
  f32 pixel_height;
  f32 ascent;
  f32 descent;
  f32 line_height;
  f32 advance;  // cell width; the face is assumed monospace

  Glyph *glyphs;  // open-addressed, keyed by codepoint
  u64 capacity;

  // The font file stays mapped for the atlas's lifetime, because the parsed
  // font points straight into it. Mapping rather than reading matters here:
  // Iosevka ships as a single 400MB collection.
  FileMapping font_file;
  void *font_info;  // stbtt_fontinfo, hidden to keep stb out of this header
  f32 scale;

  Arena *arena;
  bool valid;
};

// Loads a font and prepares an empty atlas. Returns false if the font cannot be
// read or parsed.
//
// `face_name` picks a face out of a TrueType *collection* (.ttc), which is how
// Iosevka ships -- one file holding dozens of variants, where index 0 is
// whichever the builder happened to put first. Empty means "the first face".
bool GlyphAtlasInit(GlyphAtlas *atlas, Arena *arena, SDL_Renderer *renderer, String8 font_path,
                    f32 pixel_height, String8 face_name = String8{nullptr, 0});
void GlyphAtlasDestroy(GlyphAtlas *atlas);

// Looks a glyph up, rasterising and uploading it on first use. Never returns
// null: an unrenderable codepoint yields a blank glyph of the right width so
// layout stays aligned.
const Glyph *GlyphAtlasGet(GlyphAtlas *atlas, u32 codepoint);

// UV of the opaque white pixel, for solid fills.
[[nodiscard]] RectF32 GlyphAtlasWhiteUV(const GlyphAtlas *atlas);

// Searches the usual locations for a monospace font. Returns an empty string if
// nothing suitable turns up; `out_face` receives the face to ask for when the
// file is a collection.
[[nodiscard]] String8 GlyphAtlasFindMonospaceFont(Arena *arena, String8 *out_face);
