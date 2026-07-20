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
  // With several cursors, the primary stays the brightest so "where am I" is
  // still answerable; pending marks are dimmer again, being not yet live.
  Vec4F32 cursor_secondary;
  Vec4F32 cursor_pending;
  Vec4F32 selection;
  Vec4F32 search_match;     // every `/` match while the highlight is on
  Vec4F32 current_line;     // a faint wash, not a hard highlight

  // The gutter. Dim enough to stay out of the way of the text, with the
  // cursor's own line brought back up so the count reads from a fixed point.
  Vec4F32 line_number;
  Vec4F32 line_number_current;

  Vec4F32 status_background;
  Vec4F32 status_text;
  Vec4F32 status_background_inactive;
  Vec4F32 status_text_inactive;

  Vec4F32 split_border;
  Vec4F32 message;
  Vec4F32 error;
  Vec4F32 diagnostic_error;
  Vec4F32 diagnostic_warning;
  Vec4F32 diagnostic_information;
  Vec4F32 diagnostic_hint;
  Vec4F32 popup_background;
  Vec4F32 popup_border;
  Vec4F32 popup_selected;
  Vec4F32 popup_text;
  Vec4F32 popup_detail;

  Vec4F32 syntax[(u64)TokenKind::COUNT];
};

[[nodiscard]] Theme ThemeDefault();
[[nodiscard]] inline Vec4F32 ThemeColorForToken(const Theme *theme, TokenKind kind) {
  return theme->syntax[(u64)kind];
}
