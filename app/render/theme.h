#pragma once

#include "base/base_math.h"
#include "base/base_types.h"
#include "text/token.h"

// Colours, compiled in. There is no config file by design; changing the look
// means editing this table.

struct Theme {
  Vec4F32 background;
  Vec4F32 text;

  Vec4F32 cursor;
  Vec4F32 cursor_text;      // the character sitting under a block cursor
  Vec4F32 selection;
  Vec4F32 current_line;     // a faint wash, not a hard highlight

  Vec4F32 status_background;
  Vec4F32 status_text;
  Vec4F32 status_background_inactive;
  Vec4F32 status_text_inactive;

  Vec4F32 split_border;
  Vec4F32 message;
  Vec4F32 error;

  Vec4F32 syntax[(u64)TokenKind::COUNT];
};

[[nodiscard]] Theme ThemeDefault();
[[nodiscard]] inline Vec4F32 ThemeColorForToken(const Theme *theme, TokenKind kind) {
  return theme->syntax[(u64)kind];
}
