#include "text/gap_buffer.h"

#include <string.h>

namespace {

// Logical offset -> physical index. Only valid for pos < size.
[[nodiscard]] inline u64 PhysicalFromLogical(const GapBuffer *gb, u64 pos) {
  return (pos < gb->gap_start) ? pos : pos + (gb->gap_end - gb->gap_start);
}

// Slides the gap so it begins at logical offset `pos`. This is the only
// function that shuffles text; everything else works against the gap in place.
void MoveGapTo(GapBuffer *gb, u64 pos) {
  Assert(pos <= GapBufferSize(gb));

  if (pos < gb->gap_start) {
    // Shift the bytes between pos and the gap forward, to the gap's tail.
    u64 n = gb->gap_start - pos;
    memmove(gb->data + gb->gap_end - n, gb->data + pos, n);
    gb->gap_start -= n;
    gb->gap_end -= n;
  } else if (pos > gb->gap_start) {
    // Shift the bytes after the gap backward, into the gap's head.
    u64 n = pos - gb->gap_start;
    memmove(gb->data + gb->gap_start, gb->data + gb->gap_end, n);
    gb->gap_start += n;
    gb->gap_end += n;
  }
}

// Guarantees at least `needed` free bytes in the gap.
void EnsureGap(GapBuffer *gb, u64 needed) {
  u64 gap = gb->gap_end - gb->gap_start;
  if (gap >= needed) return;

  u64 size = GapBufferSize(gb);
  u64 target = Max(gb->capacity * 2, size + needed + kGapBufferMinGap);

  // The buffer owns its arena, so its bytes are the arena's last allocation and
  // the new capacity can be appended directly to the existing block.
  Assert(ArenaPos(gb->arena) == gb->base_pos + gb->capacity &&
         "gap buffer arena must be exclusive to the buffer");

  u64 delta = target - gb->capacity;
  u8 *appended = (u8 *)ArenaPush(gb->arena, delta, 1);
  Assert(appended == gb->data + gb->capacity);
  (void)appended;

  // Slide the post-gap tail to the far end so the new space joins the gap.
  u64 tail = gb->capacity - gb->gap_end;
  if (tail) memmove(gb->data + target - tail, gb->data + gb->gap_end, tail);
  gb->gap_end = target - tail;
  gb->capacity = target;
}

}  // namespace

void GapBufferInit(GapBuffer *gb, Arena *arena, u64 initial_capacity) {
  u64 capacity = Max(initial_capacity, kGapBufferMinCapacity);

  gb->arena = arena;
  gb->base_pos = ArenaPos(arena);
  gb->data = PushArrayNoZero(arena, u8, capacity);
  gb->capacity = capacity;
  gb->gap_start = 0;
  gb->gap_end = capacity;
}

void GapBufferInitFrom(GapBuffer *gb, Arena *arena, String8 initial) {
  GapBufferInit(gb, arena, initial.size + kGapBufferMinCapacity);
  if (initial.size) {
    memcpy(gb->data, initial.str, initial.size);
    gb->gap_start = initial.size;
  }
}

void GapBufferClear(GapBuffer *gb) {
  gb->gap_start = 0;
  gb->gap_end = gb->capacity;
}

u8 GapBufferByteAt(const GapBuffer *gb, u64 pos) {
  if (pos >= GapBufferSize(gb)) return 0;
  return gb->data[PhysicalFromLogical(gb, pos)];
}

void GapBufferInsert(GapBuffer *gb, u64 pos, String8 s) {
  if (s.size == 0) return;
  Assert(pos <= GapBufferSize(gb));
  Assert((s.str + s.size <= gb->data || s.str >= gb->data + gb->capacity) &&
         "inserted text must not alias the buffer's own storage");

  MoveGapTo(gb, pos);
  EnsureGap(gb, s.size);
  memcpy(gb->data + gb->gap_start, s.str, s.size);
  gb->gap_start += s.size;
}

void GapBufferDelete(GapBuffer *gb, RangeU64 range) {
  u64 size = GapBufferSize(gb);
  u64 min = Min(range.min, size);
  u64 max = Min(range.max, size);
  if (max <= min) return;

  // Deleting is just widening the gap over the doomed bytes.
  MoveGapTo(gb, min);
  gb->gap_end += (max - min);
}

u64 GapBufferCopyInto(u8 *dst, const GapBuffer *gb, RangeU64 range) {
  u64 size = GapBufferSize(gb);
  u64 min = Min(range.min, size);
  u64 max = Min(range.max, size);
  if (max <= min) return 0;

  // The range may straddle the gap, in which case it copies as two runs.
  u64 head = (min < gb->gap_start) ? Min(max, gb->gap_start) - min : 0;
  if (head) memcpy(dst, gb->data + min, head);

  u64 tail = (max - min) - head;
  if (tail) {
    u64 tail_start = PhysicalFromLogical(gb, min + head);
    memcpy(dst + head, gb->data + tail_start, tail);
  }

  return max - min;
}

String8 GapBufferCopyRange(Arena *arena, const GapBuffer *gb, RangeU64 range) {
  u64 size = GapBufferSize(gb);
  u64 min = Min(range.min, size);
  u64 max = Min(range.max, size);
  if (max <= min) return String8{nullptr, 0};

  u64 n = max - min;
  u8 *dst = PushArrayNoZero(arena, u8, n + 1);
  GapBufferCopyInto(dst, gb, RangeU64{min, max});
  dst[n] = 0;
  return String8{dst, n};
}

String8 GapBufferCopyAll(Arena *arena, const GapBuffer *gb) {
  return GapBufferCopyRange(arena, gb, RangeU64{0, GapBufferSize(gb)});
}

u64 GapBufferFindChar(const GapBuffer *gb, u8 c, u64 start) {
  u64 size = GapBufferSize(gb);

  // Scan each physical run with memchr rather than byte-at-a-time, so this
  // stays fast enough to index a large file's lines on load.
  for (u64 pos = Min(start, size); pos < size;) {
    u64 phys = PhysicalFromLogical(gb, pos);
    u64 run_end = (pos < gb->gap_start) ? gb->gap_start : size;
    u64 run = run_end - pos;

    u8 *hit = (u8 *)memchr(gb->data + phys, c, run);
    if (hit) return pos + (u64)(hit - (gb->data + phys));
    pos = run_end;
  }

  return size;
}

u64 GapBufferFindCharReverse(const GapBuffer *gb, u8 c, u64 start) {
  u64 size = GapBufferSize(gb);
  for (u64 pos = Min(start, size); pos > 0; pos -= 1) {
    if (gb->data[PhysicalFromLogical(gb, pos - 1)] == c) return pos - 1;
  }
  return size;
}
