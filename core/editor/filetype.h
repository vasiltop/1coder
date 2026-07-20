#pragma once

#include "base/base_string.h"
#include "editor/buffer.h"

struct Editor;

// What opening a path means, keyed by extension.
//
// The explorer needs one answer to "the user pressed <CR> on this" that covers
// a directory, a text file and an image alike. Without this, every caller would
// grow its own chain of extension checks and they would drift apart.
//
// A handler returns a buffer for the path; registering none leaves the default,
// which is to open the path as text.

inline constexpr u64 kMaxFiletypeHandlers = 64;

struct FiletypeHandler {
  String8 ext;  // no leading dot; matched case-insensitively
  BufferHandle (*open)(Editor *ed, String8 path);
};

struct FiletypeRegistry {
  FiletypeHandler handlers[kMaxFiletypeHandlers];
  u64 count;
};

// Later registrations for the same extension replace earlier ones, so a
// handler can be overridden without unregistering.
void FiletypeRegister(Editor *ed, String8 ext, BufferHandle (*open)(Editor *ed, String8 path));

// Opens `path` the way its kind implies: a directory in the explorer, a
// registered extension through its handler, anything else as text. The single
// place that question is answered.
[[nodiscard]] BufferHandle FiletypeOpen(Editor *ed, String8 path);
