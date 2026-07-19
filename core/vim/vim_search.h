#pragma once

#include "editor/buffer.h"

// In-file search: what `/`, `?`, `n` and `N` are built on.
//
// Kept apart from vim_motions.cpp because a motion is a pure function of
// (buffer, position, count) and a search additionally needs a pattern, which
// lives on the editor. Separating it keeps the motion signature honest and
// leaves the searching itself as something a test can call directly.
//
// Literal substring matching only -- no regular expressions, consistent with
// the project-wide grep.

struct SearchHit {
  u64 offset;
  bool found;
  bool wrapped;  // the search ran off the end and resumed from the other side
};

// Matching is case-sensitive only when the pattern contains an uppercase
// letter, which is vim's 'smartcase'.
[[nodiscard]] bool SearchPatternIsCaseSensitive(String8 pattern);

// Finds the next match strictly after `from` (or strictly before, searching
// backward), as vim does: `/` on top of a match moves to the following one
// rather than staying put.
[[nodiscard]] SearchHit BufferSearch(const Buffer *buffer, String8 pattern, u64 from,
                                     bool forward, bool wrap);

// Every match in `range`, for highlighting the visible lines. Returns the count
// written, which is capped at `max_hits`.
[[nodiscard]] u64 BufferSearchAll(const Buffer *buffer, String8 pattern, RangeU64 range,
                                  RangeU64 *out_hits, u64 max_hits);

// The word under (or next after) the cursor, which is what `*` and `#` search
// for. Empty when the rest of the line holds no word character.
[[nodiscard]] String8 BufferWordAtCursor(Arena *arena, const Buffer *buffer, u64 pos);
