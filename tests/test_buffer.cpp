#include "editor/buffer_registry.h"
#include "test.h"

namespace {

struct Fixture {
  Arena *arena;
  BufferRegistry reg;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(16));
  BufferRegistryInit(&f.reg, f.arena, 16);
  return f;
}

void Destroy(Fixture *f) {
  BufferRegistryDestroy(&f->reg);
  ArenaRelease(f->arena);
}

Buffer *OpenWithText(Fixture *f, const char *text) {
  BufferHandle handle = BufferOpen(&f->reg, BufferKind::Scratch, Str8Lit("test"));
  Buffer *buffer = BufferFromHandle(&f->reg, handle);
  BufferSetText(nullptr, buffer, Str8C(text));
  return buffer;
}

String8 TextOf(Arena *arena, Buffer *buffer) { return BufferTextAll(arena, buffer); }

}  // namespace

TEST(buffer_set_text_establishes_baseline) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "one\ntwo\nthree");

  CHECK_EQ(BufferSize(buffer), 13);
  CHECK_EQ(BufferLineCount(buffer), 3);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("one\ntwo\nthree"));

  // Loading is not an edit: nothing to undo, and not dirty.
  CHECK(!BufferIsDirty(buffer));
  bool moved = true;
  (void)BufferUndo(nullptr, buffer, &moved);
  CHECK(!moved);

  Destroy(&f);
}

TEST(buffer_replace_updates_everything_at_once) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "one\ntwo\nthree");

  BufferReplace(nullptr, buffer, RangeU64{4, 7}, Str8Lit("TWO\nX"), 4, 9);

  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("one\nTWO\nX\nthree"));
  // The line index tracked the inserted newline without a rebuild.
  CHECK_EQ(BufferLineCount(buffer), 4);
  CHECK_EQ(BufferOffsetFromLine(buffer, 2), 8);
  CHECK(BufferIsDirty(buffer));

  Destroy(&f);
}

TEST(buffer_insert_and_delete_wrappers) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "hello");

  BufferInsert(nullptr, buffer, 5, Str8Lit(" world"), 5, 11);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("hello world"));

  BufferDelete(nullptr, buffer, RangeU64{0, 6}, 0, 0);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("world"));

  Destroy(&f);
}

TEST(buffer_replace_accepts_text_from_itself) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "abc\n");

  // Pasting a yank taken from this same buffer: the source aliases the
  // storage being written, which must not corrupt the result.
  String8 yanked = BufferTextRange(f.arena, buffer, RangeU64{0, 4});
  BufferInsert(nullptr, buffer, 4, yanked, 4, 8);

  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("abc\nabc\n"));

  Destroy(&f);
}

TEST(buffer_undo_redo_restores_text_and_cursor) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "one\ntwo\nthree");

  BufferReplace(nullptr, buffer, RangeU64{4, 7}, Str8Lit("XX"), 6, 4);

  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("one\nXX\nthree"));

  bool moved = false;
  u64 cursor = BufferUndo(nullptr, buffer, &moved);
  CHECK(moved);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("one\ntwo\nthree"));
  CHECK_EQ(cursor, 6);  // where the cursor was before the edit
  CHECK_EQ(BufferLineCount(buffer), 3);

  cursor = BufferRedo(nullptr, buffer, &moved);
  CHECK(moved);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("one\nXX\nthree"));
  CHECK_EQ(cursor, 4);

  Destroy(&f);
}

TEST(buffer_undo_group_reverts_as_one) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "");

  // An insert-mode session typed one character at a time.
  BufferBeginEditGroup(buffer);
  BufferInsert(nullptr, buffer, 0, Str8Lit("h"), 0, 1);
  BufferInsert(nullptr, buffer, 1, Str8Lit("i"), 1, 2);
  BufferInsert(nullptr, buffer, 2, Str8Lit("!"), 2, 3);
  BufferEndEditGroup(buffer);

  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("hi!"));

  bool moved = false;
  u64 cursor = BufferUndo(nullptr, buffer, &moved);
  CHECK(moved);
  CHECK_EQ(BufferSize(buffer), 0);  // the whole session, not one character
  CHECK_EQ(cursor, 0);

  (void)BufferRedo(nullptr, buffer, &moved);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("hi!"));

  Destroy(&f);
}

TEST(buffer_undo_across_multiline_edits) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "a\nb\nc\nd");

  BufferDelete(nullptr, buffer, RangeU64{0, 6}, 0, 0);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("d"));
  CHECK_EQ(BufferLineCount(buffer), 1);

  bool moved = false;
  (void)BufferUndo(nullptr, buffer, &moved);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("a\nb\nc\nd"));
  // The line index must come back with the text, not just the bytes.
  CHECK_EQ(BufferLineCount(buffer), 4);
  CHECK_EQ(BufferOffsetFromLine(buffer, 3), 6);

  Destroy(&f);
}

TEST(buffer_codepoint_stepping_and_columns) {
  Fixture f = MakeFixture();
  // "aé€b" -- 1, 2, 3 and 1 byte characters.
  Buffer *buffer = OpenWithText(&f, "a\xC3\xA9\xE2\x82\xAC" "b");

  CHECK_EQ(BufferNextCodepoint(buffer, 0), 1);
  CHECK_EQ(BufferNextCodepoint(buffer, 1), 3);  // skips the 2-byte character
  CHECK_EQ(BufferNextCodepoint(buffer, 3), 6);  // and the 3-byte one
  CHECK_EQ(BufferNextCodepoint(buffer, 7), 7);  // clamps at the end

  CHECK_EQ(BufferPrevCodepoint(buffer, 6), 3);
  CHECK_EQ(BufferPrevCodepoint(buffer, 3), 1);
  CHECK_EQ(BufferPrevCodepoint(buffer, 0), 0);

  // Columns count characters, not bytes, so the cursor's visual position is
  // right on lines containing multi-byte text.
  CHECK_EQ(BufferColumnFromOffset(buffer, 0), 0);
  CHECK_EQ(BufferColumnFromOffset(buffer, 3), 2);
  CHECK_EQ(BufferColumnFromOffset(buffer, 6), 3);
  CHECK_EQ(BufferOffsetFromColumn(buffer, 0, 2), 3);
  CHECK_EQ(BufferOffsetFromColumn(buffer, 0, 99), 7);  // clamps to line end

  Destroy(&f);
}

TEST(buffer_line_helpers) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "one\ntwo\nthree");

  CHECK_STR(BufferLineText(f.arena, buffer, 1), Str8Lit("two"));
  CHECK_EQ(BufferLineFromOffset(buffer, 5), 1);
  CHECK_EQ(BufferOffsetFromLine(buffer, 2), 8);
  CHECK_EQ(BufferLineEnd(buffer, 1), 7);

  Destroy(&f);
}

TEST(buffer_hooks_fire_on_edit) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenWithText(&f, "abc");

  // A syntax provider would hang off this hook; here it just records.
  static u32 edit_count;
  static RangeU64 last_range;
  static u64 last_new_len;
  edit_count = 0;

  buffer->hooks.on_edit = [](Editor *, Buffer *, RangeU64 range, u64 new_len) {
    edit_count += 1;
    last_range = range;
    last_new_len = new_len;
  };

  BufferReplace(nullptr, buffer, RangeU64{1, 2}, Str8Lit("XY"), 1, 3);
  CHECK_EQ(edit_count, 1);
  CHECK_EQ(last_range.min, 1);
  CHECK_EQ(last_range.max, 2);
  CHECK_EQ(last_new_len, 2);

  // Undo is an edit too, so consumers stay in step.
  bool moved = false;
  (void)BufferUndo(nullptr, buffer, &moved);
  CHECK_EQ(edit_count, 2);

  Destroy(&f);
}

TEST(buffer_registry_handles_resolve) {
  Fixture f = MakeFixture();

  BufferHandle a = BufferOpen(&f.reg, BufferKind::File, Str8Lit("a"));
  BufferHandle b = BufferOpen(&f.reg, BufferKind::File, Str8Lit("b"));

  CHECK(BufferFromHandle(&f.reg, a) != nullptr);
  CHECK(BufferFromHandle(&f.reg, b) != nullptr);
  CHECK(!BufferHandleEqual(a, b));
  CHECK_EQ(f.reg.count, 2);

  CHECK_STR(BufferFromHandle(&f.reg, a)->name, Str8Lit("a"));

  // A zero handle never resolves.
  CHECK(BufferFromHandle(&f.reg, BufferHandleZero()) == nullptr);

  Destroy(&f);
}

TEST(buffer_registry_stale_handles_are_safe) {
  Fixture f = MakeFixture();

  BufferHandle handle = BufferOpen(&f.reg, BufferKind::File, Str8Lit("doomed"));
  BufferClose(&f.reg, nullptr, handle);

  // The slot is free, so the handle must not resolve.
  CHECK(BufferFromHandle(&f.reg, handle) == nullptr);

  // Reusing the slot must not resurrect the old handle -- this is the whole
  // point of the generation counter.
  BufferHandle reused = BufferOpen(&f.reg, BufferKind::File, Str8Lit("new"));
  CHECK_EQ(reused.index, handle.index);
  CHECK(reused.generation != handle.generation);
  CHECK(BufferFromHandle(&f.reg, handle) == nullptr);
  CHECK(BufferFromHandle(&f.reg, reused) != nullptr);
  CHECK_STR(BufferFromHandle(&f.reg, reused)->name, Str8Lit("new"));

  // Closing twice is harmless.
  BufferClose(&f.reg, nullptr, handle);

  Destroy(&f);
}

TEST(buffer_registry_close_hook_runs) {
  Fixture f = MakeFixture();

  BufferHandle handle = BufferOpen(&f.reg, BufferKind::File, Str8Lit("x"));
  Buffer *buffer = BufferFromHandle(&f.reg, handle);

  static u32 closed;
  closed = 0;
  buffer->hooks.on_close = [](Editor *, Buffer *) { closed += 1; };

  BufferClose(&f.reg, nullptr, handle);
  CHECK_EQ(closed, 1);

  Destroy(&f);
}

TEST(buffer_registry_lookup_by_path_and_name) {
  Fixture f = MakeFixture();

  BufferHandle handle = BufferOpen(&f.reg, BufferKind::File, Str8Lit("main.cpp"));
  Buffer *buffer = BufferFromHandle(&f.reg, handle);
  buffer->path = PushStr8Copy(buffer->arena, Str8Lit("/src/main.cpp"));

  // Opening the same file twice must find the existing buffer rather than
  // creating a second, divergent copy.
  CHECK(BufferHandleEqual(BufferFromPath(&f.reg, Str8Lit("/src/main.cpp")), handle));
  CHECK(BufferHandleEqual(BufferFromName(&f.reg, Str8Lit("main.cpp")), handle));
  CHECK_EQ(BufferFromPath(&f.reg, Str8Lit("/other")).index, 0);
  // An empty path must not match the many buffers that have none.
  CHECK_EQ(BufferFromPath(&f.reg, Str8Lit("")).index, 0);

  Destroy(&f);
}

TEST(buffer_registry_iteration_and_wrapping) {
  Fixture f = MakeFixture();

  BufferHandle a = BufferOpen(&f.reg, BufferKind::File, Str8Lit("a"));
  BufferHandle b = BufferOpen(&f.reg, BufferKind::File, Str8Lit("b"));
  BufferHandle c = BufferOpen(&f.reg, BufferKind::File, Str8Lit("c"));

  CHECK(BufferHandleEqual(BufferFirst(&f.reg), a));
  CHECK(BufferHandleEqual(BufferNext(&f.reg, a), b));
  CHECK(BufferHandleEqual(BufferNext(&f.reg, c), BufferHandleZero()));

  // :bnext and :bprev wrap around.
  CHECK(BufferHandleEqual(BufferNextWrapping(&f.reg, c), a));
  CHECK(BufferHandleEqual(BufferPrevWrapping(&f.reg, a), c));
  CHECK(BufferHandleEqual(BufferPrevWrapping(&f.reg, b), a));

  // Iteration skips closed slots.
  BufferClose(&f.reg, nullptr, b);
  CHECK(BufferHandleEqual(BufferNext(&f.reg, a), c));

  Destroy(&f);
}

TEST(buffer_registry_full) {
  Fixture f = MakeFixture();

  // Slot 0 is reserved so a zeroed handle reads as "none", leaving capacity-1.
  for (u64 i = 0; i < 15; i += 1) {
    CHECK(BufferOpen(&f.reg, BufferKind::Scratch, Str8Lit("x")).index != 0);
  }
  CHECK_EQ(BufferOpen(&f.reg, BufferKind::Scratch, Str8Lit("overflow")).index, 0);

  Destroy(&f);
}
