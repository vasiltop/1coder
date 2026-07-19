#include "editor/buffer_registry.h"
#include "editor/view.h"
#include "test.h"

namespace {

struct Fixture {
  Arena *arena;
  BufferRegistry reg;
  Buffer *buffer;
  View view;
};

Fixture MakeFixture(String8 text) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(16));
  BufferRegistryInit(&f.reg, f.arena, 8);

  BufferHandle handle = BufferOpen(&f.reg, BufferKind::Scratch, Str8Lit("test"));
  f.buffer = BufferFromHandle(&f.reg, handle);
  BufferSetText(nullptr, f.buffer, text);

  ViewInit(&f.view, handle);
  return f;
}

// A buffer of `count` lines, each "lineN".
Fixture MakeLinesFixture(u64 count) {
  Arena *tmp = ArenaAlloc(MB(4));
  String8List list = {};
  for (u64 i = 0; i < count; i += 1) {
    Str8ListPush(tmp, &list, PushStr8F(tmp, "line%llu", (unsigned long long)i));
  }
  Fixture f = MakeFixture(Str8ListJoin(tmp, &list, Str8Lit("\n")));
  ArenaRelease(tmp);
  return f;
}

void Destroy(Fixture *f) {
  BufferRegistryDestroy(&f->reg);
  ArenaRelease(f->arena);
}

}  // namespace

TEST(view_cursor_clamps_to_mode) {
  Fixture f = MakeFixture(Str8Lit("abc\ndef"));

  // Normal mode rests on a character, so it cannot sit on the newline.
  f.view.vim.mode = VimMode::Normal;
  ViewSetCursor(&f.view, f.buffer, 3);
  CHECK_EQ(f.view.cursor, 2);

  // Insert mode may sit one past the last character, where typing would land.
  f.view.vim.mode = VimMode::Insert;
  ViewSetCursor(&f.view, f.buffer, 3);
  CHECK_EQ(f.view.cursor, 3);

  // An empty line has nowhere to back off to.
  Fixture empty = MakeFixture(Str8Lit("\nx"));
  empty.view.vim.mode = VimMode::Normal;
  ViewSetCursor(&empty.view, empty.buffer, 0);
  CHECK_EQ(empty.view.cursor, 0);
  Destroy(&empty);

  Destroy(&f);
}

TEST(view_preferred_column_survives_short_lines) {
  Fixture f = MakeFixture(Str8Lit("longer line\nab\nanother long line"));
  f.view.vim.mode = VimMode::Normal;

  // Sit at column 8 on the first line.
  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 8);
  f.view.preferred_column = 8;
  CHECK_EQ(ViewCursorColumn(&f.view, f.buffer), 8);

  // Down onto a short line: the cursor clamps but the wanted column is kept.
  ViewSetCursorKeepColumn(&f.view, f.buffer,
                          BufferOffsetFromColumn(f.buffer, 1, f.view.preferred_column));
  CHECK_EQ(ViewCursorColumn(&f.view, f.buffer), 1);  // "ab" clamped in normal mode
  CHECK_EQ(f.view.preferred_column, 8);

  // Down again onto a long line: back to the remembered column.
  ViewSetCursorKeepColumn(&f.view, f.buffer,
                          BufferOffsetFromColumn(f.buffer, 2, f.view.preferred_column));
  CHECK_EQ(ViewCursorColumn(&f.view, f.buffer), 8);

  // A horizontal move updates the remembered column.
  ViewSetCursor(&f.view, f.buffer, BufferOffsetFromColumn(f.buffer, 2, 3));
  CHECK_EQ(f.view.preferred_column, 3);

  Destroy(&f);
}

TEST(view_selection) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  // No selection outside visual mode.
  f.view.vim.mode = VimMode::Normal;
  f.view.cursor = 5;
  CHECK(RangeEmpty(ViewSelection(&f.view, f.buffer)));

  // Characterwise includes the character under the cursor.
  f.view.vim.mode = VimMode::Visual;
  f.view.vim.visual_anchor = 4;
  f.view.cursor = 6;
  RangeU64 sel = ViewSelection(&f.view, f.buffer);
  CHECK_EQ(sel.min, 4);
  CHECK_EQ(sel.max, 7);

  // It works backwards too.
  f.view.vim.visual_anchor = 6;
  f.view.cursor = 4;
  sel = ViewSelection(&f.view, f.buffer);
  CHECK_EQ(sel.min, 4);
  CHECK_EQ(sel.max, 7);

  // Linewise covers whole lines including the newline.
  f.view.vim.mode = VimMode::VisualLine;
  f.view.vim.visual_anchor = 5;
  f.view.cursor = 5;
  sel = ViewSelection(&f.view, f.buffer);
  CHECK_EQ(sel.min, 4);
  CHECK_EQ(sel.max, 8);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Vertical scrolling
// ---------------------------------------------------------------------------

TEST(view_scroll_follows_cursor_down_and_up) {
  Fixture f = MakeLinesFixture(100);
  const i32 height = 20, width = 80;

  // A cursor within the first screenful needs no scrolling.
  ViewSetCursorLineColumn(&f.view, f.buffer, 5, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_line, 0);

  // Moving past the bottom scrolls just enough to reveal the cursor line.
  ViewSetCursorLineColumn(&f.view, f.buffer, 25, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_line, 6);  // 25 - 20 + 1
  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));

  // Coming back up scrolls the other way, again minimally.
  ViewSetCursorLineColumn(&f.view, f.buffer, 3, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_line, 3);

  Destroy(&f);
}

TEST(view_scroll_keeps_context_margin) {
  Fixture f = MakeLinesFixture(100);
  const i32 height = 20, width = 80;
  const i32 scrolloff = 3;

  // Scrolling down stops with `scrolloff` lines still below the cursor.
  ViewSetCursorLineColumn(&f.view, f.buffer, 30, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, scrolloff, 0);
  CHECK_EQ(f.view.scroll_line, 14);  // 30 + 3 - 20 + 1

  // And with `scrolloff` lines above it going the other way.
  ViewSetCursorLineColumn(&f.view, f.buffer, 16, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, scrolloff, 0);
  CHECK_EQ(f.view.scroll_line, 13);

  Destroy(&f);
}

TEST(view_scroll_margin_relaxes_at_buffer_end) {
  Fixture f = MakeLinesFixture(30);
  const i32 height = 20, width = 80;

  // There is no margin to be had below the last line, so the view must settle
  // rather than scrolling into empty space.
  ViewSetCursorLineColumn(&f.view, f.buffer, 29, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 5, 0);
  CHECK_EQ(f.view.scroll_line, 10);  // last line at the bottom, no further
  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));

  Destroy(&f);
}

TEST(view_scroll_no_op_when_buffer_fits) {
  Fixture f = MakeLinesFixture(5);
  const i32 height = 20, width = 80;

  ViewSetCursorLineColumn(&f.view, f.buffer, 4, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 3, 0);
  // Nothing to scroll when the whole buffer is on screen.
  CHECK_EQ(f.view.scroll_line, 0);

  Destroy(&f);
}

TEST(view_scroll_margin_shrinks_in_tiny_viewport) {
  Fixture f = MakeLinesFixture(100);
  const i32 height = 3, width = 80;

  // A margin larger than the viewport would fight itself; it must be reduced
  // rather than leaving the cursor permanently off screen.
  ViewSetCursorLineColumn(&f.view, f.buffer, 50, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 10, 0);
  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));

  Destroy(&f);
}

TEST(view_scroll_lines_leaves_cursor_until_it_must_move) {
  Fixture f = MakeLinesFixture(100);
  const i32 height = 20;

  ViewSetCursorLineColumn(&f.view, f.buffer, 10, 0);

  // Scrolling by a little keeps the cursor where it was.
  ViewScrollLines(&f.view, f.buffer, 5, height);
  CHECK_EQ(f.view.scroll_line, 5);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 10);

  // Scrolling past it drags the cursor to the top visible line.
  ViewScrollLines(&f.view, f.buffer, 10, height);
  CHECK_EQ(f.view.scroll_line, 15);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 15);

  // And the other way, to the bottom visible line.
  ViewSetCursorLineColumn(&f.view, f.buffer, 30, 0);
  ViewScrollLines(&f.view, f.buffer, -20, height);
  CHECK_EQ(f.view.scroll_line, 0);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 19);

  Destroy(&f);
}

TEST(view_scroll_clamps_at_buffer_bounds) {
  Fixture f = MakeLinesFixture(50);
  const i32 height = 20;

  ViewScrollLines(&f.view, f.buffer, -100, height);
  CHECK_EQ(f.view.scroll_line, 0);  // cannot scroll above the first line

  ViewScrollLines(&f.view, f.buffer, 1000, height);
  CHECK_EQ(f.view.scroll_line, 30);  // last line lands at the bottom

  Destroy(&f);
}

TEST(view_scroll_clamps_after_buffer_shrinks) {
  Fixture f = MakeLinesFixture(100);
  const i32 height = 20;

  ViewScrollLines(&f.view, f.buffer, 70, height);
  CHECK_EQ(f.view.scroll_line, 70);

  // An edit deletes most of the buffer out from under the scrolled view.
  BufferDelete(nullptr, f.buffer, RangeU64{20, BufferSize(f.buffer)}, 0, 0);
  ViewClampScroll(&f.view, f.buffer, height);

  CHECK(f.view.scroll_line + (u64)height <= BufferLineCount(f.buffer) ||
        f.view.scroll_line == 0);

  Destroy(&f);
}

TEST(view_half_and_full_page_scrolling) {
  Fixture f = MakeLinesFixture(200);
  const i32 height = 20;

  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 0);

  ViewScrollHalfPage(&f.view, f.buffer, 1, height);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 10);

  ViewScrollHalfPage(&f.view, f.buffer, -1, height);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 0);

  // A full page keeps two lines of overlap for continuity.
  ViewScrollFullPage(&f.view, f.buffer, 1, height);
  CHECK_EQ(ViewCursorLine(&f.view, f.buffer), 18);

  Destroy(&f);
}

TEST(view_center_top_bottom) {
  Fixture f = MakeLinesFixture(200);
  const i32 height = 20;

  ViewSetCursorLineColumn(&f.view, f.buffer, 100, 0);

  ViewCenterOnCursor(&f.view, f.buffer, height);
  CHECK_EQ(f.view.scroll_line, 90);

  ViewCursorLineToTop(&f.view, f.buffer, height);
  CHECK_EQ(f.view.scroll_line, 100);

  ViewCursorLineToBottom(&f.view, f.buffer, height);
  CHECK_EQ(f.view.scroll_line, 81);

  Destroy(&f);
}

TEST(view_visible_lines_clips_to_buffer) {
  Fixture f = MakeLinesFixture(10);

  RangeU64 visible = ViewVisibleLines(&f.view, f.buffer, 20);
  CHECK_EQ(visible.min, 0);
  CHECK_EQ(visible.max, 10);  // never past the last line

  f.view.scroll_line = 5;
  visible = ViewVisibleLines(&f.view, f.buffer, 3);
  CHECK_EQ(visible.min, 5);
  CHECK_EQ(visible.max, 8);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Horizontal scrolling
// ---------------------------------------------------------------------------

TEST(view_horizontal_scroll_follows_cursor) {
  Fixture f = MakeFixture(Str8Lit(
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"));
  const i32 width = 40, height = 10;

  // Within the first screenful, no horizontal scrolling.
  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 10);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_column, 0);

  // Moving right off the edge scrolls just enough.
  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 60);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_column, 21);  // 60 - 40 + 1
  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));

  // Back to the start scrolls all the way left again.
  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 0);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_column, 0);

  Destroy(&f);
}

TEST(view_horizontal_scroll_keeps_side_margin) {
  Fixture f = MakeFixture(Str8Lit(
      "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"));
  const i32 width = 40, height = 10;
  const i32 side = 8;

  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 60);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, side);
  CHECK_EQ(f.view.scroll_column, 29);  // 60 + 8 - 40 + 1

  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 32);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, side);
  CHECK_EQ(f.view.scroll_column, 24);  // 32 - 8

  Destroy(&f);
}

TEST(view_horizontal_scroll_explicit) {
  Fixture f = MakeFixture(Str8Lit(
      "0123456789012345678901234567890123456789012345678901234567890123456789"));
  const i32 width = 20;

  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 0);

  ViewScrollColumns(&f.view, f.buffer, 10, width);
  CHECK_EQ(f.view.scroll_column, 10);
  // The cursor was scrolled off the left, so it comes along.
  CHECK_EQ(ViewCursorColumn(&f.view, f.buffer), 10);

  ViewScrollColumns(&f.view, f.buffer, -100, width);
  CHECK_EQ(f.view.scroll_column, 0);  // clamps at the left edge

  Destroy(&f);
}

TEST(view_horizontal_scroll_counts_codepoints_not_bytes) {
  // A line of 3-byte characters: horizontal position must be measured in
  // characters, or the cursor and the scroll window would disagree.
  Arena *tmp = ArenaAlloc(MB(1));
  String8List list = {};
  for (u64 i = 0; i < 50; i += 1) Str8ListPush(tmp, &list, Str8Lit("\xE2\x82\xAC"));
  Fixture f = MakeFixture(Str8ListJoin(tmp, &list, String8{nullptr, 0}));
  ArenaRelease(tmp);

  const i32 width = 20, height = 10;

  CHECK_EQ(BufferSize(f.buffer), 150);  // 50 characters, 150 bytes

  ViewSetCursorLineColumn(&f.view, f.buffer, 0, 40);
  CHECK_EQ(ViewCursorColumn(&f.view, f.buffer), 40);

  ViewScrollToCursor(&f.view, f.buffer, width, height, 0, 0);
  CHECK_EQ(f.view.scroll_column, 21);  // in columns, not bytes
  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));

  Destroy(&f);
}

TEST(view_max_visible_line_length) {
  Fixture f = MakeFixture(Str8Lit("short\na much longer line here\nmid line"));

  CHECK_EQ(ViewMaxVisibleLineLength(&f.view, f.buffer, 10), 23);

  // Only the visible region counts, which is what a scrollbar should reflect.
  f.view.scroll_line = 2;
  CHECK_EQ(ViewMaxVisibleLineLength(&f.view, f.buffer, 10), 8);

  Destroy(&f);
}

TEST(view_scroll_both_axes_together) {
  Arena *tmp = ArenaAlloc(MB(4));
  String8List list = {};
  for (u64 i = 0; i < 100; i += 1) {
    Str8ListPush(tmp, &list,
                 PushStr8F(tmp, "%03llu_0123456789012345678901234567890123456789012345678901234567890123456789",
                           (unsigned long long)i));
  }
  Fixture f = MakeFixture(Str8ListJoin(tmp, &list, Str8Lit("\n")));
  ArenaRelease(tmp);

  const i32 width = 30, height = 15;

  // A diagonal jump must resolve both axes in one call.
  ViewSetCursorLineColumn(&f.view, f.buffer, 60, 55);
  ViewScrollToCursor(&f.view, f.buffer, width, height, 2, 4);

  CHECK(ViewIsCursorVisible(&f.view, f.buffer, width, height));
  CHECK(f.view.scroll_line > 0);
  CHECK(f.view.scroll_column > 0);

  Destroy(&f);
}
