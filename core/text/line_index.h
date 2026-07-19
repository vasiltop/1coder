#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_types.h"
#include "text/gap_buffer.h"

// Byte offset of the first character of every line, so line/offset conversion
// is a binary search rather than a scan. Without it, drawing a viewport or
// moving the cursor down a line would cost a walk from the top of the file.
//
// Lines are newline-*separated*: text with N newlines has N+1 lines, so a
// trailing "\n" yields a final empty line. An empty buffer is one empty line,
// meaning `count` is never 0 and `line_starts[0]` is always 0.
//
// Like GapBuffer, a LineIndex owns its arena exclusively so it can extend its
// array in place instead of orphaning the old one in the bump allocator.

struct LineIndex {
  u64 *line_starts;
  u64 count;
  u64 capacity;
  Arena *arena;
  u64 base_pos;
};

inline constexpr u64 kLineIndexMinCapacity = 256;

void LineIndexInit(LineIndex *idx, Arena *arena, u64 initial_capacity = kLineIndexMinCapacity);

// Full rescan. Used on load and as the fallback whenever an edit's extent is
// not known.
void LineIndexRebuild(LineIndex *idx, const GapBuffer *gb);

// Incremental patch after an edit that replaced `old_range` with `new_len`
// bytes. `gb` must already reflect the edit. Equivalent in result to
// LineIndexRebuild, but touches only the affected lines plus a shift of the
// tail.
void LineIndexEdit(LineIndex *idx, const GapBuffer *gb, RangeU64 old_range, u64 new_len);

[[nodiscard]] inline u64 LineCount(const LineIndex *idx) { return idx->count; }

// Both clamp: an offset past the end maps to the last line, and a line past
// the end maps to the last line's start.
[[nodiscard]] u64 LineFromOffset(const LineIndex *idx, u64 offset);
[[nodiscard]] u64 OffsetFromLine(const LineIndex *idx, u64 line);

// Offset just past the line's last character, excluding its newline.
[[nodiscard]] u64 LineEndOffset(const LineIndex *idx, const GapBuffer *gb, u64 line);

// Line content without its newline.
[[nodiscard]] RangeU64 LineRange(const LineIndex *idx, const GapBuffer *gb, u64 line);
// Line content including its newline, if it has one.
[[nodiscard]] RangeU64 LineRangeWithNewline(const LineIndex *idx, const GapBuffer *gb, u64 line);

[[nodiscard]] inline u64 LineLength(const LineIndex *idx, const GapBuffer *gb, u64 line) {
  return RangeSize(LineRange(idx, gb, line));
}
