#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_string.h"
#include "base/base_types.h"

// A gap buffer: one contiguous allocation with a movable hole at the last edit
// point. Insertion and deletion near the gap are memcpy-cheap, which is exactly
// the access pattern of a text editor.
//
// Positions in the public API are always *logical* byte offsets into the text.
// The gap is an implementation detail and never appears in an offset.
//
// A GapBuffer owns its arena exclusively -- its bytes must be the only live
// allocation in it -- so growth can extend the existing block in place instead
// of reallocating and orphaning the old one in the bump allocator.

struct GapBuffer {
  u8 *data;
  u64 capacity;
  u64 gap_start;  // physical; text occupies [0, gap_start)
  u64 gap_end;    // physical; text resumes at [gap_end, capacity)
  Arena *arena;
  u64 base_pos;   // arena position of `data`, used to verify exclusive ownership
};

inline constexpr u64 kGapBufferMinCapacity = KB(4);
inline constexpr u64 kGapBufferMinGap = 64;

void GapBufferInit(GapBuffer *gb, Arena *arena, u64 initial_capacity = kGapBufferMinCapacity);
void GapBufferInitFrom(GapBuffer *gb, Arena *arena, String8 initial);
void GapBufferClear(GapBuffer *gb);

[[nodiscard]] inline u64 GapBufferSize(const GapBuffer *gb) {
  return gb->capacity - (gb->gap_end - gb->gap_start);
}

// Out-of-range reads return 0 rather than asserting, so scanning loops can
// probe one past the end without guarding every access.
[[nodiscard]] u8 GapBufferByteAt(const GapBuffer *gb, u64 pos);

// `s` must not alias the gap buffer's own storage.
void GapBufferInsert(GapBuffer *gb, u64 pos, String8 s);
void GapBufferDelete(GapBuffer *gb, RangeU64 range);

// Ranges are clamped to the buffer, so callers may pass loose bounds.
[[nodiscard]] String8 GapBufferCopyRange(Arena *arena, const GapBuffer *gb, RangeU64 range);
[[nodiscard]] String8 GapBufferCopyAll(Arena *arena, const GapBuffer *gb);
// Copies into caller-provided storage; returns bytes written.
u64 GapBufferCopyInto(u8 *dst, const GapBuffer *gb, RangeU64 range);

// Byte search, used by line indexing and by vim's f/t and search motions.
// Returns GapBufferSize() when absent.
[[nodiscard]] u64 GapBufferFindChar(const GapBuffer *gb, u8 c, u64 start);
[[nodiscard]] u64 GapBufferFindCharReverse(const GapBuffer *gb, u8 c, u64 start);
