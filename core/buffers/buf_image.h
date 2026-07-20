#pragma once

#include "editor/buffer.h"

struct Editor;

// Images as buffers.
//
// Core reads headers only -- dimensions, format, size -- and never pixels.
// Decoding needs a texture, and a texture needs a graphics API, which core is
// not allowed to know exists. So an image buffer carries a path and metadata,
// and the app layer is what eventually turns that into something drawn.
//
// Until it does, the buffer's text is a readable summary, which means the
// feature degrades to something useful rather than to a screen of binary: every
// motion, `y`, and the status line all work, because it is still a buffer.

struct ImageInfo {
  String8 path;
  String8 format;  // "PNG", "JPEG", ... ; empty when unrecognised
  u64 byte_size;
  u32 width;   // 0 when the header could not be read
  u32 height;
};

// Reads `path`'s header and opens a read-only buffer describing it. Reuses an
// existing buffer for the same path, as EditorOpenFile does.
[[nodiscard]] BufferHandle ImageBufferOpen(Editor *ed, String8 path);

// The metadata for an image buffer, or null for any other buffer. This is what
// a renderer reads to size the image on screen.
[[nodiscard]] const ImageInfo *ImageBufferInfo(const Buffer *buffer);

// Registers the handlers that route image extensions here.
void ImageRegisterFiletypes(Editor *ed);
