#include "text/undo.h"

namespace {

void EnsureCapacity(UndoStack *undo, u64 needed) {
  if (undo->capacity >= needed) return;

  u64 target = Max(undo->capacity * 2, needed);

  Assert(ArenaPos(undo->record_arena) ==
             undo->base_pos + undo->capacity * sizeof(UndoRecord) &&
         "undo record arena must be exclusive to the record array");

  u64 delta = (target - undo->capacity) * sizeof(UndoRecord);
  u8 *appended = (u8 *)ArenaPush(undo->record_arena, delta, alignof(UndoRecord));
  Assert(appended == (u8 *)(undo->records + undo->capacity));
  (void)appended;

  undo->capacity = target;
}

// Drops everything from `pos` onward, reclaiming its text.
void TruncateToPos(UndoStack *undo) {
  if (undo->pos >= undo->count) return;
  ArenaPopTo(undo->text_arena, undo->records[undo->pos].text_arena_pos);
  undo->count = undo->pos;
  if (undo->group_start_count > undo->count) undo->group_start_count = undo->count;
}

}  // namespace

void UndoInit(UndoStack *undo, Arena *record_arena, Arena *text_arena, u64 initial_capacity) {
  u64 capacity = Max(initial_capacity, kUndoMinCapacity);

  undo->record_arena = record_arena;
  undo->text_arena = text_arena;
  undo->base_pos = ArenaPos(record_arena);
  undo->records = PushArrayNoZero(record_arena, UndoRecord, capacity);
  undo->capacity = capacity;
  undo->count = 0;
  undo->pos = 0;
  undo->next_group = 1;
  undo->open_group = 0;
  undo->group_open = false;
}

void UndoClear(UndoStack *undo) {
  undo->pos = 0;
  TruncateToPos(undo);
  undo->group_open = false;
  undo->next_group = 1;
  undo->group_start_count = 0;
}

void UndoBeginGroup(UndoStack *undo) {
  if (undo->group_open) return;
  undo->group_open = true;
  undo->open_group = undo->next_group;
  undo->next_group += 1;
  undo->group_start_count = undo->count;
}

void UndoEndGroup(UndoStack *undo) { undo->group_open = false; }

void UndoPush(UndoStack *undo, RangeU64 range, String8 old_text, String8 new_text,
              u64 cursor_before, u64 cursor_after, bool fn_before, bool fn_after) {
  // Editing after an undo abandons the redo branch.
  TruncateToPos(undo);

  EnsureCapacity(undo, undo->count + 1);

  u64 text_pos = ArenaPos(undo->text_arena);

  UndoRecord *rec = &undo->records[undo->count];
  rec->range = range;
  rec->old_text = PushStr8Copy(undo->text_arena, old_text);
  rec->new_text = PushStr8Copy(undo->text_arena, new_text);
  rec->cursor_before = cursor_before;
  rec->cursor_after = cursor_after;
  rec->fn_before = fn_before;
  rec->fn_after = fn_after;
  rec->text_arena_pos = text_pos;

  if (undo->group_open) {
    rec->group = undo->open_group;
  } else {
    rec->group = undo->next_group;
    undo->next_group += 1;
  }

  undo->count += 1;
  undo->pos = undo->count;
}

UndoStep UndoStepUndo(UndoStack *undo) {
  if (undo->pos == 0) return UndoStep{nullptr, 0, 0};

  // Walk back over every record sharing the group of the most recent one.
  u32 group = undo->records[undo->pos - 1].group;
  u64 first = undo->pos;
  while (first > 0 && undo->records[first - 1].group == group) first -= 1;

  UndoStep step = {};
  step.records = &undo->records[first];
  step.count = undo->pos - first;
  step.cursor = undo->records[first].cursor_before;
  step.final_newline = undo->records[first].fn_before;

  undo->pos = first;
  // A group cannot stay open across an undo, or the next edit would join it.
  undo->group_open = false;
  return step;
}

UndoStep UndoStepRedo(UndoStack *undo) {
  if (undo->pos >= undo->count) return UndoStep{nullptr, 0, 0};

  u32 group = undo->records[undo->pos].group;
  u64 last = undo->pos;
  while (last < undo->count && undo->records[last].group == group) last += 1;

  UndoStep step = {};
  step.records = &undo->records[undo->pos];
  step.count = last - undo->pos;
  step.cursor = undo->records[last - 1].cursor_after;
  step.final_newline = undo->records[last - 1].fn_after;

  undo->pos = last;
  undo->group_open = false;
  return step;
}
