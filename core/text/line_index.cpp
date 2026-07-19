#include "text/line_index.h"

#include <string.h>

namespace {

void EnsureCapacity(LineIndex *idx, u64 needed) {
  if (idx->capacity >= needed) return;

  u64 target = Max(idx->capacity * 2, needed);

  Assert(ArenaPos(idx->arena) == idx->base_pos + idx->capacity * sizeof(u64) &&
         "line index arena must be exclusive to the index");

  u64 delta = (target - idx->capacity) * sizeof(u64);
  u8 *appended = (u8 *)ArenaPush(idx->arena, delta, alignof(u64));
  Assert(appended == (u8 *)(idx->line_starts + idx->capacity));
  (void)appended;

  idx->capacity = target;
}

// Number of newlines in [range.min, range.max).
[[nodiscard]] u64 CountNewlines(const GapBuffer *gb, RangeU64 range) {
  u64 count = 0;
  for (u64 p = range.min; p < range.max;) {
    u64 nl = GapBufferFindChar(gb, '\n', p);
    if (nl >= range.max) break;
    count += 1;
    p = nl + 1;
  }
  return count;
}

}  // namespace

void LineIndexInit(LineIndex *idx, Arena *arena, u64 initial_capacity) {
  u64 capacity = Max(initial_capacity, kLineIndexMinCapacity);

  idx->arena = arena;
  idx->base_pos = ArenaPos(arena);
  idx->line_starts = PushArrayNoZero(arena, u64, capacity);
  idx->capacity = capacity;
  idx->count = 1;
  idx->line_starts[0] = 0;
}

void LineIndexRebuild(LineIndex *idx, const GapBuffer *gb) {
  u64 size = GapBufferSize(gb);

  idx->count = 1;
  idx->line_starts[0] = 0;

  for (u64 p = 0; p < size;) {
    u64 nl = GapBufferFindChar(gb, '\n', p);
    if (nl >= size) break;
    EnsureCapacity(idx, idx->count + 1);
    idx->line_starts[idx->count] = nl + 1;
    idx->count += 1;
    p = nl + 1;
  }
}

void LineIndexEdit(LineIndex *idx, const GapBuffer *gb, RangeU64 old_range, u64 new_len) {
  u64 new_size = GapBufferSize(gb);
  u64 start = old_range.min;
  u64 old_len = RangeSize(old_range);
  i64 delta = (i64)new_len - (i64)old_len;

  // Rescan from the start of the first line the edit touched...
  u64 first_line = LineFromOffset(idx, start);
  u64 s = idx->line_starts[first_line];

  // ...through the end of the line the edit now ends in. Extending to the next
  // newline is what makes the boundary case work when the edit itself ends with
  // a newline: the line start just past it gets regenerated rather than being
  // left behind as a stale shifted entry.
  u64 after = start + new_len;
  u64 nl = GapBufferFindChar(gb, '\n', after);
  u64 region_end = (nl >= new_size) ? new_size : nl + 1;

  // The same boundary in pre-edit coordinates, so we know which old entries the
  // rescan supersedes.
  u64 old_region_end = (u64)((i64)region_end - delta);

  u64 last_line = first_line;
  while (last_line + 1 < idx->count && idx->line_starts[last_line + 1] <= old_region_end) {
    last_line += 1;
  }

  u64 region_lines = CountNewlines(gb, RangeU64{s, region_end});
  u64 tail_count = idx->count - (last_line + 1);
  u64 new_count = first_line + 1 + region_lines + tail_count;

  EnsureCapacity(idx, new_count);

  // Slide the untouched tail into place first; memmove copes with either
  // direction of overlap.
  u64 tail_dst = first_line + 1 + region_lines;
  if (tail_count && tail_dst != last_line + 1) {
    memmove(&idx->line_starts[tail_dst], &idx->line_starts[last_line + 1],
            tail_count * sizeof(u64));
  }
  for (u64 i = 0; i < tail_count; i += 1) {
    idx->line_starts[tail_dst + i] = (u64)((i64)idx->line_starts[tail_dst + i] + delta);
  }

  // Regenerate the rescanned region's starts.
  u64 write = first_line + 1;
  for (u64 p = s; p < region_end;) {
    u64 hit = GapBufferFindChar(gb, '\n', p);
    if (hit >= region_end) break;
    idx->line_starts[write] = hit + 1;
    write += 1;
    p = hit + 1;
  }
  Assert(write == first_line + 1 + region_lines);

  idx->count = new_count;
}

u64 LineFromOffset(const LineIndex *idx, u64 offset) {
  // Largest line whose start is <= offset.
  u64 lo = 0, hi = idx->count - 1;
  while (lo < hi) {
    u64 mid = lo + (hi - lo + 1) / 2;
    if (idx->line_starts[mid] <= offset) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

u64 OffsetFromLine(const LineIndex *idx, u64 line) {
  u64 clamped = Min(line, idx->count - 1);
  return idx->line_starts[clamped];
}

u64 LineEndOffset(const LineIndex *idx, const GapBuffer *gb, u64 line) {
  u64 clamped = Min(line, idx->count - 1);
  if (clamped + 1 < idx->count) {
    // Next line's start is one past this line's newline.
    return idx->line_starts[clamped + 1] - 1;
  }
  return GapBufferSize(gb);
}

RangeU64 LineRange(const LineIndex *idx, const GapBuffer *gb, u64 line) {
  return RangeU64{OffsetFromLine(idx, line), LineEndOffset(idx, gb, line)};
}

RangeU64 LineRangeWithNewline(const LineIndex *idx, const GapBuffer *gb, u64 line) {
  u64 clamped = Min(line, idx->count - 1);
  u64 start = idx->line_starts[clamped];
  u64 end = (clamped + 1 < idx->count) ? idx->line_starts[clamped + 1] : GapBufferSize(gb);
  return RangeU64{start, end};
}
