#include "test.h"
#include "text/undo.h"

namespace {

struct Fixture {
  Arena *record_arena;
  Arena *text_arena;
  UndoStack undo;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.record_arena = ArenaAlloc(MB(16));
  f.text_arena = ArenaAlloc(MB(16));
  UndoInit(&f.undo, f.record_arena, f.text_arena);
  return f;
}

void Destroy(Fixture *f) {
  ArenaRelease(f->record_arena);
  ArenaRelease(f->text_arena);
}

// Convenience: record an insertion of `text` at `at`.
void PushInsert(Fixture *f, u64 at, String8 text) {
  UndoPush(&f->undo, RangeU64{at, at}, String8{nullptr, 0}, text, at, at + text.size);
}

}  // namespace

TEST(undo_starts_empty) {
  Fixture f = MakeFixture();
  CHECK(!UndoCanUndo(&f.undo));
  CHECK(!UndoCanRedo(&f.undo));
  CHECK_EQ(UndoStepUndo(&f.undo).count, 0);
  CHECK_EQ(UndoStepRedo(&f.undo).count, 0);
  Destroy(&f);
}

TEST(undo_single_record_round_trip) {
  Fixture f = MakeFixture();

  UndoPush(&f.undo, RangeU64{2, 5}, Str8Lit("abc"), Str8Lit("XY"), 2, 4);
  CHECK(UndoCanUndo(&f.undo));
  CHECK(!UndoCanRedo(&f.undo));

  UndoStep step = UndoStepUndo(&f.undo);
  CHECK_EQ(step.count, 1);
  CHECK_EQ(step.cursor, 2);
  CHECK_EQ(step.records[0].range.min, 2);
  CHECK_STR(step.records[0].old_text, Str8Lit("abc"));
  CHECK_STR(step.records[0].new_text, Str8Lit("XY"));

  CHECK(!UndoCanUndo(&f.undo));
  CHECK(UndoCanRedo(&f.undo));

  UndoStep redo = UndoStepRedo(&f.undo);
  CHECK_EQ(redo.count, 1);
  CHECK_EQ(redo.cursor, 4);
  CHECK(UndoCanUndo(&f.undo));
  CHECK(!UndoCanRedo(&f.undo));

  Destroy(&f);
}

TEST(undo_copies_text_so_callers_can_pass_views) {
  Fixture f = MakeFixture();

  // Push from a mutable buffer, then scribble over it. The record must have
  // taken its own copy, since callers pass views into the buffer being edited.
  u8 scratch[4] = {'a', 'b', 'c', 0};
  UndoPush(&f.undo, RangeU64{0, 3}, String8{scratch, 3}, Str8Lit("z"), 0, 1);
  scratch[0] = 'X';
  scratch[1] = 'Y';
  scratch[2] = 'Z';

  UndoStep step = UndoStepUndo(&f.undo);
  CHECK_STR(step.records[0].old_text, Str8Lit("abc"));

  Destroy(&f);
}

TEST(undo_ungrouped_records_step_individually) {
  Fixture f = MakeFixture();

  PushInsert(&f, 0, Str8Lit("a"));
  PushInsert(&f, 1, Str8Lit("b"));
  PushInsert(&f, 2, Str8Lit("c"));

  // Each push without an open group is its own step.
  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK(!UndoCanUndo(&f.undo));

  Destroy(&f);
}

TEST(undo_group_steps_atomically) {
  Fixture f = MakeFixture();

  // An insert-mode session: many keystrokes, one undo.
  UndoBeginGroup(&f.undo);
  PushInsert(&f, 0, Str8Lit("h"));
  PushInsert(&f, 1, Str8Lit("i"));
  PushInsert(&f, 2, Str8Lit("!"));
  UndoEndGroup(&f.undo);

  UndoStep step = UndoStepUndo(&f.undo);
  CHECK_EQ(step.count, 3);
  CHECK_EQ(step.cursor, 0);  // cursor_before of the first record in the group
  // Records come back in apply order, so undo walks them backwards.
  CHECK_STR(step.records[0].new_text, Str8Lit("h"));
  CHECK_STR(step.records[2].new_text, Str8Lit("!"));
  CHECK(!UndoCanUndo(&f.undo));

  UndoStep redo = UndoStepRedo(&f.undo);
  CHECK_EQ(redo.count, 3);
  CHECK_EQ(redo.cursor, 3);  // cursor_after of the last record

  Destroy(&f);
}

TEST(undo_groups_do_not_merge_with_neighbours) {
  Fixture f = MakeFixture();

  PushInsert(&f, 0, Str8Lit("x"));  // ungrouped

  UndoBeginGroup(&f.undo);
  PushInsert(&f, 1, Str8Lit("a"));
  PushInsert(&f, 2, Str8Lit("b"));
  UndoEndGroup(&f.undo);

  PushInsert(&f, 3, Str8Lit("y"));  // ungrouped

  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK_EQ(UndoStepUndo(&f.undo).count, 2);
  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK(!UndoCanUndo(&f.undo));

  Destroy(&f);
}

TEST(undo_groups_nest) {
  Fixture f = MakeFixture();

  // An inner group closing must not end the outer one: this is what lets a
  // multi-cursor pass wrap edits that already group themselves.
  UndoBeginGroup(&f.undo);
  PushInsert(&f, 0, Str8Lit("a"));

  UndoBeginGroup(&f.undo);
  PushInsert(&f, 1, Str8Lit("b"));
  UndoEndGroup(&f.undo);

  // Still inside the outer group, so this joins it too.
  PushInsert(&f, 2, Str8Lit("c"));
  UndoEndGroup(&f.undo);

  PushInsert(&f, 3, Str8Lit("z"));  // outside, must be its own step

  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);
  CHECK_EQ(UndoStepUndo(&f.undo).count, 3);
  CHECK(!UndoCanUndo(&f.undo));

  Destroy(&f);
}

TEST(undo_unmatched_end_group_is_ignored) {
  Fixture f = MakeFixture();

  UndoEndGroup(&f.undo);  // nothing open; must not underflow the depth

  UndoBeginGroup(&f.undo);
  PushInsert(&f, 0, Str8Lit("a"));
  PushInsert(&f, 1, Str8Lit("b"));
  UndoEndGroup(&f.undo);

  CHECK_EQ(UndoStepUndo(&f.undo).count, 2);

  Destroy(&f);
}

TEST(undo_push_discards_redo_branch) {
  Fixture f = MakeFixture();

  PushInsert(&f, 0, Str8Lit("a"));
  PushInsert(&f, 1, Str8Lit("b"));
  PushInsert(&f, 2, Str8Lit("c"));

  (void)UndoStepUndo(&f.undo);
  (void)UndoStepUndo(&f.undo);
  CHECK(UndoCanRedo(&f.undo));
  CHECK_EQ(f.undo.count, 3);

  // A new edit here abandons the "b"/"c" branch.
  PushInsert(&f, 1, Str8Lit("Z"));
  CHECK(!UndoCanRedo(&f.undo));
  CHECK_EQ(f.undo.count, 2);

  UndoStep step = UndoStepUndo(&f.undo);
  CHECK_STR(step.records[0].new_text, Str8Lit("Z"));

  Destroy(&f);
}

TEST(undo_discarded_branch_reclaims_text_storage) {
  Fixture f = MakeFixture();

  PushInsert(&f, 0, Str8Lit("first"));
  u64 after_first = ArenaPos(f.text_arena);

  // A large record, then undo past it and overwrite the branch.
  Arena *tmp = ArenaAlloc(MB(4));
  String8 big = PushStr8F(tmp, "%0100000d", 1);
  UndoPush(&f.undo, RangeU64{0, 0}, String8{nullptr, 0}, big, 0, big.size);
  CHECK(ArenaPos(f.text_arena) > after_first + 100000);

  (void)UndoStepUndo(&f.undo);
  PushInsert(&f, 0, Str8Lit("x"));

  // The abandoned record's text must not stay resident.
  CHECK(ArenaPos(f.text_arena) < after_first + 1000);

  ArenaRelease(tmp);
  Destroy(&f);
}

TEST(undo_stack_grows_past_capacity) {
  Fixture f = MakeFixture();

  u64 count = kUndoMinCapacity * 3;
  for (u64 i = 0; i < count; i += 1) PushInsert(&f, i, Str8Lit("x"));

  CHECK_EQ(f.undo.count, count);
  CHECK(f.undo.capacity >= count);

  // Every record must still be individually reachable after the array moved.
  for (u64 i = 0; i < count; i += 1) {
    UndoStep step = UndoStepUndo(&f.undo);
    CHECK_EQ(step.count, 1);
    CHECK_EQ(step.records[0].range.min, count - 1 - i);
  }
  CHECK(!UndoCanUndo(&f.undo));

  Destroy(&f);
}

TEST(undo_clear_resets_everything) {
  Fixture f = MakeFixture();

  UndoBeginGroup(&f.undo);
  PushInsert(&f, 0, Str8Lit("a"));
  PushInsert(&f, 1, Str8Lit("b"));

  UndoClear(&f.undo);
  CHECK(!UndoCanUndo(&f.undo));
  CHECK(!UndoCanRedo(&f.undo));
  CHECK_EQ(f.undo.count, 0);

  // The group must not survive the clear and swallow the next edit.
  PushInsert(&f, 0, Str8Lit("x"));
  PushInsert(&f, 1, Str8Lit("y"));
  CHECK_EQ(UndoStepUndo(&f.undo).count, 1);

  Destroy(&f);
}

TEST(undo_interleaved_undo_redo_sequence) {
  Fixture f = MakeFixture();

  PushInsert(&f, 0, Str8Lit("a"));
  PushInsert(&f, 1, Str8Lit("b"));
  PushInsert(&f, 2, Str8Lit("c"));

  CHECK_STR(UndoStepUndo(&f.undo).records[0].new_text, Str8Lit("c"));
  CHECK_STR(UndoStepRedo(&f.undo).records[0].new_text, Str8Lit("c"));
  CHECK_STR(UndoStepUndo(&f.undo).records[0].new_text, Str8Lit("c"));
  CHECK_STR(UndoStepUndo(&f.undo).records[0].new_text, Str8Lit("b"));
  CHECK_STR(UndoStepRedo(&f.undo).records[0].new_text, Str8Lit("b"));
  CHECK_STR(UndoStepRedo(&f.undo).records[0].new_text, Str8Lit("c"));
  CHECK(!UndoCanRedo(&f.undo));

  Destroy(&f);
}
