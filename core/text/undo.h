#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Undo history as a flat list of replacements. Each record says "at `range`,
// `old_text` became `new_text`", which is enough to run in either direction.
//
// This layer deliberately knows nothing about gap buffers: it stores and orders
// records, and the buffer layer applies them. That keeps the history testable
// on its own and keeps one apply path in the buffer.
//
// Records carry a group id so that one user-visible action -- a `dw`, or an
// entire insert-mode session -- undoes atomically even though it was recorded
// as several edits.

struct UndoRecord {
  RangeU64 range;
  String8 old_text;
  String8 new_text;
  u64 cursor_before;
  u64 cursor_after;
  u32 group;
  u64 text_arena_pos;
};

// A contiguous run of records forming one undoable action. Records are in the
// order they were applied: undo walks them backwards, redo forwards.
struct UndoStep {
  UndoRecord *records;
  u64 count;
  u64 cursor;
};

struct UndoStack {
  UndoRecord *records;
  u64 count;
  u64 capacity;
  u64 pos;
  u32 next_group;
  u32 open_group;
  b32 group_open;
  Arena *record_arena;
  Arena *text_arena;
  u64 base_pos;
};

inline constexpr u64 kUndoMinCapacity = 256;

void UndoInit(UndoStack *undo, Arena *record_arena, Arena *text_arena,
              u64 initial_capacity = kUndoMinCapacity);

// Discards all history. Text storage is reclaimed.
void UndoClear(UndoStack *undo);

// Records sharing an open group undo together. Groups do not nest; a second
// Begin without an End is ignored.
void UndoBeginGroup(UndoStack *undo);
void UndoEndGroup(UndoStack *undo);

// Records an edit. Copies both texts into the stack's own storage, so callers
// may pass views into the buffer being edited. Pushing discards any redo
// history, which is what makes editing after an undo behave as expected.
void UndoPush(UndoStack *undo, RangeU64 range, String8 old_text, String8 new_text,
              u64 cursor_before, u64 cursor_after);

[[nodiscard]] inline bool UndoCanUndo(const UndoStack *undo) { return undo->pos > 0; }
[[nodiscard]] inline bool UndoCanRedo(const UndoStack *undo) { return undo->pos < undo->count; }

// Move one group backwards/forwards. Returns a step with count == 0 when there
// is nothing to do.
[[nodiscard]] UndoStep UndoStepUndo(UndoStack *undo);
[[nodiscard]] UndoStep UndoStepRedo(UndoStack *undo);
