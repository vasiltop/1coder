#include "editor/command.h"
#include "editor/editor.h"
#include "editor/multicursor.h"
#include "editor/view.h"
#include "test.h"

// Multi-cursor, driven through the keyboard path like the vim tests, so what is
// exercised is the real fan-out in CommandExecArgs rather than a shortcut into
// the cursor list.

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
};

Fixture MakeFixture(const char *text) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  BufferSetText(&f.ed, buffer, Str8C(text));

  View *view = EditorFocusedView(&f.ed);
  ViewSetCursor(view, buffer, 0);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  ArenaRelease(f->arena);
}

void Type(Fixture *f, const char *spec) { EditorProcessSpec(&f->ed, spec); }

String8 TextOf(Fixture *f) { return BufferTextAll(f->arena, EditorFocusedBuffer(&f->ed)); }

View *ViewOf(Fixture *f) { return EditorFocusedView(&f->ed); }
Buffer *BufferOf(Fixture *f) { return EditorFocusedBuffer(&f->ed); }

u64 CountOf(Fixture *f) { return ViewCursorCount(ViewOf(f)); }

// Every cursor offset, primary included, in ascending order.
void CursorOffsets(Fixture *f, u64 *out, u64 *count) {
  View *view = ViewOf(f);
  MultiCursorNormalize(view, BufferOf(f));

  u64 n = 0;
  out[n++] = view->cursor;
  for (u64 i = 0; i < view->extra_count; i += 1) out[n++] = view->extras[i].offset;

  for (u64 i = 1; i < n; i += 1) {
    u64 v = out[i];
    u64 j = i;
    while (j > 0 && out[j - 1] > v) {
      out[j] = out[j - 1];
      j -= 1;
    }
    out[j] = v;
  }
  *count = n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

TEST(multicursor_placement_flow) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  // <leader>mc starts placing and marks the first position; motions move
  // between positions, `c` marks each one; <CR> makes them live.
  Type(&f, "<leader>mc");
  CHECK(ViewOf(&f)->placing);
  CHECK_EQ(ViewOf(&f)->pending_count, 1);

  Type(&f, "jcjc");
  CHECK_EQ(ViewOf(&f)->pending_count, 3);
  // Nothing is live until confirmed.
  CHECK_EQ(CountOf(&f), 1);

  Type(&f, "<CR>");
  CHECK(!ViewOf(&f)->placing);
  CHECK_EQ(CountOf(&f), 3);

  u64 offsets[8];
  u64 count = 0;
  CursorOffsets(&f, offsets, &count);
  CHECK_EQ(count, 3);
  CHECK_EQ(offsets[0], 0);  // line 0
  CHECK_EQ(offsets[1], 4);  // line 1
  CHECK_EQ(offsets[2], 8);  // line 2

  Destroy(&f);
}

TEST(multicursor_placement_uses_ordinary_motions) {
  Fixture f = MakeFixture("alpha beta\ngamma delta");

  // The placement map is parented to the normal map, so w, j and $ all still
  // work while aiming.
  Type(&f, "<leader>mcwcj$c<CR>");
  CHECK_EQ(CountOf(&f), 3);

  u64 offsets[8];
  u64 count = 0;
  CursorOffsets(&f, offsets, &count);
  CHECK_EQ(offsets[0], 0);   // "alpha"
  CHECK_EQ(offsets[1], 6);   // "beta"
  CHECK_EQ(offsets[2], 21);  // last character of "delta"

  Destroy(&f);
}

TEST(multicursor_placement_mark_toggles_off) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjc");
  CHECK_EQ(ViewOf(&f)->pending_count, 2);

  // Marking the same position again takes it back.
  Type(&f, "c");
  CHECK_EQ(ViewOf(&f)->pending_count, 1);

  Type(&f, "<CR>");
  CHECK_EQ(CountOf(&f), 1);

  Destroy(&f);
}

TEST(multicursor_placement_marks_line_end) {
  Fixture f = MakeFixture("aaa\nbbb");

  // The whole point: one cursor past the last character of a line and one at
  // the start of another. Normal mode cannot rest on the first of those, so `A`
  // marks it without going there.
  Type(&f, "<leader>mc");   // marks 0
  Type(&f, "c");            // unmark 0, so only the two below are marked
  Type(&f, "A");            // end of line 0, offset 3
  Type(&f, "jc");           // start of line 1, offset 4
  Type(&f, "<CR>");
  CHECK_EQ(CountOf(&f), 2);

  u64 offsets[8];
  u64 count = 0;
  CursorOffsets(&f, offsets, &count);
  CHECK_EQ(offsets[0], 3);
  CHECK_EQ(offsets[1], 4);

  // Both positions take the insertion, the end-of-line one included.
  Type(&f, "iX<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("aaaX\nXbbb"));

  Destroy(&f);
}

TEST(multicursor_placement_cancel) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<Esc>");
  CHECK(!ViewOf(&f)->placing);
  CHECK_EQ(ViewOf(&f)->pending_count, 0);
  CHECK_EQ(CountOf(&f), 1);

  // And the editor is back to ordinary normal mode: one x, one deletion, at
  // the position the motions left the cursor on.
  Type(&f, "x");
  CHECK_STR(TextOf(&f), Str8Lit("aaa\nbbb\ncc"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Fan-out
// ---------------------------------------------------------------------------

TEST(multicursor_typing_fans_out) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  CHECK_EQ(CountOf(&f), 3);

  Type(&f, "ix<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("xaaa\nxbbb\nxccc"));
  // The cursors survive the edit.
  CHECK_EQ(CountOf(&f), 3);

  Destroy(&f);
}

TEST(multicursor_typing_multiple_characters) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "ifoo<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("fooaaa\nfoobbb\nfooccc"));

  Destroy(&f);
}

TEST(multicursor_same_line_descending) {
  Fixture f = MakeFixture("alpha beta gamma");

  // Two cursors on one line is the case that breaks if edits are not applied
  // highest-offset first: the earlier insertion would shift the later cursor.
  Type(&f, "<leader>mcwcwc<CR>");
  CHECK_EQ(CountOf(&f), 3);

  Type(&f, "iX<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("Xalpha Xbeta Xgamma"));

  Destroy(&f);
}

TEST(multicursor_motion_fans_out) {
  Fixture f = MakeFixture("alpha one\nbeta two\ngamma three");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "w");

  u64 offsets[8];
  u64 count = 0;
  CursorOffsets(&f, offsets, &count);
  CHECK_EQ(count, 3);
  CHECK_EQ(offsets[0], 6);   // "one"
  CHECK_EQ(offsets[1], 15);  // "two"
  CHECK_EQ(offsets[2], 25);  // "three"

  Destroy(&f);
}

TEST(multicursor_operator_fans_out) {
  Fixture f = MakeFixture("alpha one\nbeta two\ngamma three");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\nthree"));

  Destroy(&f);
}

TEST(multicursor_change_word_fans_out) {
  Fixture f = MakeFixture("alpha one\nbeta two\ngamma three");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "ciwZZ<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("ZZ one\nZZ two\nZZ three"));

  Destroy(&f);
}

TEST(multicursor_delete_char_fans_out) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "x");
  CHECK_STR(TextOf(&f), Str8Lit("aa\nbb\ncc"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

TEST(multicursor_undo_is_one_step) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "x");
  CHECK_STR(TextOf(&f), Str8Lit("aa\nbb\ncc"));

  // Three edits, one undo: the whole pass is a single step.
  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("aaa\nbbb\nccc"));

  Destroy(&f);
}

TEST(multicursor_insert_session_is_one_step) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "iabc<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("abcaaa\nabcbbb\nabcccc"));

  // Nine insertions across three cursors, still one undo.
  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("aaa\nbbb\nccc"));

  Destroy(&f);
}

TEST(multicursor_undo_runs_once) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "x");
  Type(&f, "x");
  CHECK_STR(TextOf(&f), Str8Lit("a\nb\nc"));

  // Undo is Single: one press steps back one pass, not one per cursor.
  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("aa\nbb\ncc"));
  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("aaa\nbbb\nccc"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Cursor set upkeep
// ---------------------------------------------------------------------------

TEST(multicursor_esc_collapses) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  CHECK_EQ(CountOf(&f), 3);

  // In normal mode Esc drops the extra cursors...
  Type(&f, "<Esc>");
  CHECK_EQ(CountOf(&f), 1);
  CHECK_EQ(ViewOf(&f)->vim.mode, VimMode::Normal);

  Destroy(&f);
}

TEST(multicursor_esc_leaves_insert_before_collapsing) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "ix");

  // The first Esc ends the insert session at every cursor and keeps the set,
  // so a second edit can follow without placing them again.
  Type(&f, "<Esc>");
  CHECK_EQ(ViewOf(&f)->vim.mode, VimMode::Normal);
  CHECK_EQ(CountOf(&f), 3);
  CHECK_STR(TextOf(&f), Str8Lit("xaaa\nxbbb\nxccc"));

  // Only the second Esc collapses.
  Type(&f, "<Esc>");
  CHECK_EQ(CountOf(&f), 1);

  Destroy(&f);
}

TEST(multicursor_merge_on_collision) {
  Fixture f = MakeFixture("abcdef");

  // Two cursors one column apart, walked into each other.
  Type(&f, "<leader>mclc<CR>");
  CHECK_EQ(CountOf(&f), 2);

  Type(&f, "h");
  // The left cursor cannot move past the start of the line, so both end up on
  // column 0 and become one.
  CHECK_EQ(CountOf(&f), 1);

  Destroy(&f);
}

TEST(multicursor_duplicate_placement_is_one_cursor) {
  Fixture f = MakeFixture("aaa\nbbb");

  // Confirming a mark that coincides with the primary must not leave two
  // cursors on the same offset.
  ViewPlacementBegin(ViewOf(&f));
  ViewPlacementToggleMark(ViewOf(&f));
  ViewPlacementConfirm(ViewOf(&f), BufferOf(&f));
  CHECK_EQ(CountOf(&f), 1);

  Destroy(&f);
}

TEST(multicursor_survives_buffer_shrinking) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  CHECK_EQ(CountOf(&f), 3);

  // Deleting every line leaves the cursors nowhere to be; they must clamp into
  // the buffer rather than escape it.
  Type(&f, "<Esc>ggdGdd");

  View *view = ViewOf(&f);
  Buffer *buffer = BufferOf(&f);
  CHECK(view->cursor <= BufferSize(buffer));
  for (u64 i = 0; i < view->extra_count; i += 1) {
    CHECK(view->extras[i].offset <= BufferSize(buffer));
  }

  Destroy(&f);
}

TEST(multicursor_single_commands_run_once) {
  Fixture f = MakeFixture("aaa\nbbb\nccc");

  Type(&f, "<leader>mcjcjc<CR>");
  CHECK_EQ(CountOf(&f), 3);

  // A window split is a property of the window, not of any cursor: three
  // cursors must not make three splits.
  Type(&f, "<C-w>v");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Destroy(&f);
}

TEST(multicursor_dot_repeat_does_not_square) {
  Fixture f = MakeFixture("alpha one\nbeta two\ngamma three");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\nthree"));

  // `.` replays the chords, which fan out on their own. Repeating the *outer*
  // command as well would delete a word per cursor per cursor.
  Type(&f, ".");
  CHECK_STR(TextOf(&f), Str8Lit("\n\n"));

  Destroy(&f);
}


// ---------------------------------------------------------------------------
// Selections
// ---------------------------------------------------------------------------

// The selection spans of every cursor, primary included, ascending.
void SelectionSpans(Fixture *f, RangeU64 *out, u64 *count) {
  View *view = ViewOf(f);
  Buffer *buffer = BufferOf(f);
  u64 n = 0;
  out[n++] = ViewSelection(view, buffer);
  for (u64 i = 0; i < view->extra_count; i += 1) {
    out[n++] = ViewSelectionFor(view, buffer, view->extras[i].offset, view->extras[i].anchor);
  }
  for (u64 i = 1; i < n; i += 1) {
    RangeU64 v = out[i];
    u64 j = i;
    while (j > 0 && out[j - 1].min > v.min) { out[j] = out[j - 1]; j -= 1; }
    out[j] = v;
  }
  *count = n;
}

TEST(multicursor_visual_selects_per_cursor) {
  Fixture f = MakeFixture("alpha one\nbeta two\ngamma three");

  Type(&f, "<leader>mcjcjc<CR>");
  // Each cursor selects its own three characters.
  Type(&f, "vll");

  RangeU64 spans[8];
  u64 count = 0;
  SelectionSpans(&f, spans, &count);
  CHECK_EQ(count, 3);
  for (u64 i = 0; i < count; i += 1) CHECK_EQ(spans[i].max - spans[i].min, 3);

  // The operator acts on every selection.
  Type(&f, "d");
  CHECK_STR(TextOf(&f), Str8Lit("ha one\na two\nma three"));
  // And drops back to normal mode with the cursors intact.
  CHECK_EQ(ViewOf(&f)->vim.mode, VimMode::Normal);
  CHECK_EQ(CountOf(&f), 3);

  Destroy(&f);
}

TEST(multicursor_visual_yank_and_change) {
  Fixture f = MakeFixture("alpha\nbravo\ncharlie");

  Type(&f, "<leader>mcjcjc<CR>");
  Type(&f, "vllc__<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("__ha\n__vo\n__rlie"));

  Destroy(&f);
}

TEST(multicursor_overlapping_selections_merge) {
  Fixture f = MakeFixture("abcdefgh");

  // Two cursors two columns apart, each selecting three characters to the
  // right: their spans overlap, so they must collapse to one selection rather
  // than delete the shared middle twice.
  Type(&f, "<leader>mcllc<CR>");
  CHECK_EQ(CountOf(&f), 2);

  Type(&f, "vlll");
  CHECK_EQ(CountOf(&f), 1);

  RangeU64 spans[8];
  u64 count = 0;
  SelectionSpans(&f, spans, &count);
  CHECK_EQ(count, 1);

  Type(&f, "d");
  // Columns 0..5 were covered by the union; g and h remain.
  CHECK_STR(TextOf(&f), Str8Lit("gh"));

  Destroy(&f);
}

TEST(multicursor_visual_line_fans_out) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour\nfive\nsix");

  // A cursor on line 0 and one on line 2; each linewise-selects itself and the
  // line below, then both pairs are deleted.
  Type(&f, "<leader>mcjjc<CR>");
  Type(&f, "Vjd");
  CHECK_STR(TextOf(&f), Str8Lit("five\nsix"));

  Destroy(&f);
}

