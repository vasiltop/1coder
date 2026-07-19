#include "editor/command.h"
#include "editor/editor.h"
#include "test.h"

// These drive the editor the way a keyboard does -- through EditorProcessChord
// -- so they exercise the keymap, the count parser, the operator machinery and
// the buffer in one path. No window is involved.

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

String8 TextOf(Fixture *f) {
  return BufferTextAll(f->arena, EditorFocusedBuffer(&f->ed));
}

View *ViewOf(Fixture *f) { return EditorFocusedView(&f->ed); }
Buffer *BufferOf(Fixture *f) { return EditorFocusedBuffer(&f->ed); }

u64 CursorLine(Fixture *f) { return ViewCursorLine(ViewOf(f), BufferOf(f)); }
u64 CursorColumn(Fixture *f) { return ViewCursorColumn(ViewOf(f), BufferOf(f)); }
VimMode ModeOf(Fixture *f) { return ViewOf(f)->vim.mode; }

}  // namespace

// ---------------------------------------------------------------------------
// Motions
// ---------------------------------------------------------------------------

TEST(vim_basic_hjkl) {
  Fixture f = MakeFixture("abc\ndef\nghi");

  Type(&f, "l");
  CHECK_EQ(CursorColumn(&f), 1);
  Type(&f, "j");
  CHECK_EQ(CursorLine(&f), 1);
  CHECK_EQ(CursorColumn(&f), 1);
  Type(&f, "h");
  CHECK_EQ(CursorColumn(&f), 0);
  Type(&f, "k");
  CHECK_EQ(CursorLine(&f), 0);

  // h stops at the start of the line rather than wrapping.
  Type(&f, "h");
  CHECK_EQ(CursorColumn(&f), 0);
  CHECK_EQ(CursorLine(&f), 0);

  Destroy(&f);
}

TEST(vim_cursor_cannot_rest_on_newline) {
  Fixture f = MakeFixture("abc\ndef");

  Type(&f, "llll");
  // Normal mode sits on a character, so the far right of "abc" is column 2.
  CHECK_EQ(CursorColumn(&f), 2);

  Destroy(&f);
}

TEST(vim_word_motions) {
  Fixture f = MakeFixture("foo bar  baz");

  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 4);
  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 9);
  Type(&f, "b");
  CHECK_EQ(CursorColumn(&f), 4);
  Type(&f, "e");
  CHECK_EQ(CursorColumn(&f), 6);  // last character of "bar"

  Destroy(&f);
}

TEST(vim_word_motions_stop_at_punctuation) {
  Fixture f = MakeFixture("foo(bar).baz");

  // w stops at each class transition, so punctuation is its own word.
  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 3);  // "("
  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 4);  // "bar"
  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 7);  // ")."

  // W ignores punctuation entirely, so the whole thing is one WORD.
  Type(&f, "0W");
  CHECK_EQ(CursorColumn(&f), 11);

  Destroy(&f);
}

TEST(vim_line_motions) {
  Fixture f = MakeFixture("    indented line");

  Type(&f, "$");
  CHECK_EQ(CursorColumn(&f), 16);
  Type(&f, "0");
  CHECK_EQ(CursorColumn(&f), 0);
  Type(&f, "^");
  CHECK_EQ(CursorColumn(&f), 4);  // first non-blank, not first column

  Destroy(&f);
}

TEST(vim_file_motions) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour");

  Type(&f, "G");
  CHECK_EQ(CursorLine(&f), 3);
  Type(&f, "gg");
  CHECK_EQ(CursorLine(&f), 0);
  // A count on G is a line number.
  Type(&f, "3G");
  CHECK_EQ(CursorLine(&f), 2);

  Destroy(&f);
}

TEST(vim_find_char_on_line) {
  Fixture f = MakeFixture("the quick brown fox");

  Type(&f, "fq");
  CHECK_EQ(CursorColumn(&f), 4);
  Type(&f, "fo");
  CHECK_EQ(CursorColumn(&f), 12);
  Type(&f, "Fq");
  CHECK_EQ(CursorColumn(&f), 4);

  // t stops one short of the target.
  Type(&f, "0tq");
  CHECK_EQ(CursorColumn(&f), 3);

  // A character that is not on the line leaves the cursor alone.
  Type(&f, "0fz");
  CHECK_EQ(CursorColumn(&f), 0);

  Destroy(&f);
}

TEST(vim_counts_apply_to_motions) {
  Fixture f = MakeFixture("one two three four five");

  Type(&f, "3w");
  CHECK_EQ(CursorColumn(&f), 14);  // "four"
  Type(&f, "2b");
  CHECK_EQ(CursorColumn(&f), 4);

  Fixture lines = MakeFixture("a\nb\nc\nd\ne\nf");
  Type(&lines, "4j");
  CHECK_EQ(CursorLine(&lines), 4);
  Type(&lines, "2k");
  CHECK_EQ(CursorLine(&lines), 2);
  Destroy(&lines);

  Destroy(&f);
}

TEST(vim_preferred_column_across_lines) {
  Fixture f = MakeFixture("longer line here\nab\nanother long line");

  Type(&f, "10l");
  CHECK_EQ(CursorColumn(&f), 10);

  // Down onto a short line clamps, but the wanted column is remembered.
  Type(&f, "j");
  CHECK_EQ(CursorColumn(&f), 1);

  // Down again onto a long line returns to it.
  Type(&f, "j");
  CHECK_EQ(CursorColumn(&f), 10);

  Destroy(&f);
}

TEST(vim_matching_bracket) {
  Fixture f = MakeFixture("if (a[0] == b) { x }");

  Type(&f, "%");
  CHECK_EQ(CursorColumn(&f), 13);  // the ")" matching "("
  Type(&f, "%");
  CHECK_EQ(CursorColumn(&f), 3);   // and back

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Operators composed with motions
// ---------------------------------------------------------------------------

TEST(vim_delete_with_motions) {
  Fixture f = MakeFixture("foo bar baz");

  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("bar baz"));

  Type(&f, "d$");
  CHECK_STR(TextOf(&f), Str8Lit(""));

  Destroy(&f);
}

TEST(vim_delete_with_count) {
  Fixture f = MakeFixture("one two three four");

  // The count multiplies through the operator to the motion.
  Type(&f, "3dw");
  CHECK_STR(TextOf(&f), Str8Lit("four"));

  Destroy(&f);
}

TEST(vim_count_before_and_after_operator_multiply) {
  Fixture f = MakeFixture("a b c d e f g h i");

  // 2d3w deletes six words, as vim multiplies the two counts.
  Type(&f, "2d3w");
  CHECK_STR(TextOf(&f), Str8Lit("g h i"));

  Destroy(&f);
}

TEST(vim_delete_line) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  // dd is `d` in normal mode followed by `d` in operator-pending -- no special
  // case for the doubled form.
  Type(&f, "jdd");
  CHECK_STR(TextOf(&f), Str8Lit("one\nthree"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 2);

  Destroy(&f);
}

TEST(vim_delete_multiple_lines_with_count) {
  Fixture f = MakeFixture("a\nb\nc\nd\ne");

  Type(&f, "3dd");
  CHECK_STR(TextOf(&f), Str8Lit("d\ne"));

  Destroy(&f);
}

TEST(vim_delete_linewise_motion_takes_whole_lines) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour");

  // A linewise motion expands to whole lines regardless of the column.
  Type(&f, "ll");
  Type(&f, "dj");
  CHECK_STR(TextOf(&f), Str8Lit("three\nfour"));

  Destroy(&f);
}

TEST(vim_delete_inclusive_vs_exclusive_motions) {
  Fixture f = MakeFixture("hello world");

  // `e` is inclusive, so the last character of the word goes too.
  Type(&f, "de");
  CHECK_STR(TextOf(&f), Str8Lit(" world"));

  Fixture g = MakeFixture("hello world");
  // `w` is exclusive of its target.
  Type(&g, "dw");
  CHECK_STR(TextOf(&g), Str8Lit("world"));
  Destroy(&g);

  Fixture h = MakeFixture("a,b,c");
  // f is inclusive, so `df,` takes the comma.
  Type(&h, "df,");
  CHECK_STR(TextOf(&h), Str8Lit("b,c"));
  Destroy(&h);

  Destroy(&f);
}

TEST(vim_change_enters_insert_mode) {
  Fixture f = MakeFixture("foo bar");

  Type(&f, "cw");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Insert);
  CHECK_STR(TextOf(&f), Str8Lit("bar"));

  Type(&f, "new ");
  CHECK_STR(TextOf(&f), Str8Lit("new bar"));

  Type(&f, "<Esc>");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  Destroy(&f);
}

TEST(vim_change_line_keeps_the_line) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  Type(&f, "jcc");
  // cc empties the line but keeps it, unlike dd which removes it.
  CHECK_STR(TextOf(&f), Str8Lit("one\n\nthree"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 3);
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Insert);

  Type(&f, "TWO<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("one\nTWO\nthree"));

  Destroy(&f);
}

TEST(vim_yank_and_paste) {
  Fixture f = MakeFixture("foo bar");

  Type(&f, "yw");
  CHECK_STR(TextOf(&f), Str8Lit("foo bar"));  // yank does not modify

  Type(&f, "$p");
  CHECK_STR(TextOf(&f), Str8Lit("foo barfoo "));

  Destroy(&f);
}

TEST(vim_yank_line_and_paste_linewise) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  Type(&f, "yy");
  Type(&f, "jp");
  // A linewise register pastes onto its own line below.
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\none\nthree"));

  Destroy(&f);
}

TEST(vim_paste_before_and_with_count) {
  Fixture f = MakeFixture("ab");

  Type(&f, "ylP");
  CHECK_STR(TextOf(&f), Str8Lit("aab"));

  Fixture g = MakeFixture("x");
  Type(&g, "yl3p");
  CHECK_STR(TextOf(&g), Str8Lit("xxxx"));
  Destroy(&g);

  Destroy(&f);
}

TEST(vim_delete_yanks_into_register) {
  Fixture f = MakeFixture("foo bar");

  // A delete fills the unnamed register, so p pastes what was just removed.
  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("bar"));
  Type(&f, "$p");
  CHECK_STR(TextOf(&f), Str8Lit("barfoo "));

  Destroy(&f);
}

TEST(vim_indent_and_dedent) {
  Fixture f = MakeFixture("one\ntwo");

  // One shift width, which matches 'shiftwidth' from the nvim config.
  Type(&f, ">>");
  CHECK_STR(TextOf(&f), Str8Lit("  one\ntwo"));
  Type(&f, "<<");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo"));

  // An operator with a linewise motion indents the whole span.
  Type(&f, ">j");
  CHECK_STR(TextOf(&f), Str8Lit("  one\n  two"));

  Destroy(&f);
}

TEST(vim_failed_motion_aborts_operator) {
  Fixture f = MakeFixture("abc");

  // `dh` at the start of a line has nowhere to go, so nothing is deleted and
  // the editor returns to normal mode rather than staying stuck.
  Type(&f, "dh");
  CHECK_STR(TextOf(&f), Str8Lit("abc"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  // A search for an absent character likewise leaves the text alone.
  Type(&f, "dfz");
  CHECK_STR(TextOf(&f), Str8Lit("abc"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  Destroy(&f);
}

TEST(vim_escape_cancels_pending_operator) {
  Fixture f = MakeFixture("abc def");

  Type(&f, "d");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::OperatorPending);

  Type(&f, "<Esc>");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  // The abandoned operator must not attach itself to the next motion.
  Type(&f, "w");
  CHECK_STR(TextOf(&f), Str8Lit("abc def"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Insert mode
// ---------------------------------------------------------------------------

TEST(vim_insert_variants) {
  Fixture f = MakeFixture("bcd");

  Type(&f, "ia<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("abcd"));

  Type(&f, "A!<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("abcd!"));

  Type(&f, "0");
  Type(&f, "lax<Esc>");  // append after the cursor
  CHECK_STR(TextOf(&f), Str8Lit("abxcd!"));

  Destroy(&f);
}

TEST(vim_insert_at_first_non_blank) {
  Fixture f = MakeFixture("    text");

  Type(&f, "$IX<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("    Xtext"));

  Destroy(&f);
}

TEST(vim_open_lines) {
  Fixture f = MakeFixture("one\nthree");

  Type(&f, "otwo<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\nthree"));

  Type(&f, "ggOzero<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("zero\none\ntwo\nthree"));

  Destroy(&f);
}

TEST(vim_insert_mode_typing_and_backspace) {
  Fixture f = MakeFixture("");

  Type(&f, "ihello");
  CHECK_STR(TextOf(&f), Str8Lit("hello"));

  Type(&f, "<BS><BS>");
  CHECK_STR(TextOf(&f), Str8Lit("hel"));

  Type(&f, "<CR>world<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("hel\nworld"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 2);

  Destroy(&f);
}

TEST(vim_insert_utf8) {
  Fixture f = MakeFixture("");

  Type(&f, "i\xC3\xA9\xE2\x82\xAC<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("\xC3\xA9\xE2\x82\xAC"));
  // Two characters, five bytes.
  CHECK_EQ(BufferSize(BufferOf(&f)), 5);
  CHECK_EQ(CursorColumn(&f), 1);

  Destroy(&f);
}

TEST(vim_digits_are_text_in_insert_mode) {
  Fixture f = MakeFixture("");

  // A digit is a count in normal mode but plain text in insert mode.
  Type(&f, "i123<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("123"));

  Destroy(&f);
}

TEST(vim_replace_mode_overwrites) {
  Fixture f = MakeFixture("abcdef");

  Type(&f, "RXY<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("XYcdef"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Simple edits
// ---------------------------------------------------------------------------

TEST(vim_delete_char) {
  Fixture f = MakeFixture("abcdef");

  Type(&f, "x");
  CHECK_STR(TextOf(&f), Str8Lit("bcdef"));
  Type(&f, "3x");
  CHECK_STR(TextOf(&f), Str8Lit("ef"));

  Destroy(&f);
}

TEST(vim_delete_and_change_to_line_end) {
  Fixture f = MakeFixture("hello world\nsecond");

  Type(&f, "5lD");
  CHECK_STR(TextOf(&f), Str8Lit("hello\nsecond"));

  Type(&f, "0C bye<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit(" bye\nsecond"));

  Destroy(&f);
}

TEST(vim_join_lines) {
  Fixture f = MakeFixture("one\n    two\nthree");

  Type(&f, "J");
  // The leading whitespace of the joined line collapses to a single space.
  CHECK_STR(TextOf(&f), Str8Lit("one two\nthree"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Visual mode
// ---------------------------------------------------------------------------

TEST(vim_visual_select_and_delete) {
  Fixture f = MakeFixture("hello world");

  Type(&f, "vlll");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Visual);

  RangeU64 selection = ViewSelection(ViewOf(&f), BufferOf(&f));
  CHECK_EQ(selection.min, 0);
  CHECK_EQ(selection.max, 4);  // includes the character under the cursor

  Type(&f, "d");
  CHECK_STR(TextOf(&f), Str8Lit("o world"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  Destroy(&f);
}

TEST(vim_visual_line_mode) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour");

  Type(&f, "jVj");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::VisualLine);

  Type(&f, "d");
  CHECK_STR(TextOf(&f), Str8Lit("one\nfour"));

  Destroy(&f);
}

TEST(vim_visual_yank_and_change) {
  Fixture f = MakeFixture("abc def");

  Type(&f, "vlly");
  Type(&f, "$p");
  CHECK_STR(TextOf(&f), Str8Lit("abc defabc"));

  Fixture g = MakeFixture("abc def");
  Type(&g, "vllcXY<Esc>");
  CHECK_STR(TextOf(&g), Str8Lit("XY def"));
  Destroy(&g);

  Destroy(&f);
}

TEST(vim_visual_backwards_selection) {
  Fixture f = MakeFixture("hello world");

  // Anchor at the end and extend leftwards.
  Type(&f, "4lvhhh");
  RangeU64 selection = ViewSelection(ViewOf(&f), BufferOf(&f));
  CHECK_EQ(selection.min, 1);
  CHECK_EQ(selection.max, 5);

  Type(&f, "d");
  CHECK_STR(TextOf(&f), Str8Lit("h world"));

  Destroy(&f);
}

TEST(vim_visual_escape_leaves_text_alone) {
  Fixture f = MakeFixture("hello");

  Type(&f, "vll<Esc>");
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);
  CHECK_STR(TextOf(&f), Str8Lit("hello"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Undo and repeat
// ---------------------------------------------------------------------------

TEST(vim_undo_redo) {
  Fixture f = MakeFixture("one two three");

  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("two three"));

  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("one two three"));

  Type(&f, "<C-r>");
  CHECK_STR(TextOf(&f), Str8Lit("two three"));

  Destroy(&f);
}

TEST(vim_undo_treats_insert_session_as_one_step) {
  Fixture f = MakeFixture("x");

  Type(&f, "ihello<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("hellox"));

  // The whole session undoes at once, not one character at a time.
  Type(&f, "u");
  CHECK_STR(TextOf(&f), Str8Lit("x"));

  Destroy(&f);
}

TEST(vim_undo_restores_cursor) {
  Fixture f = MakeFixture("one two three");

  Type(&f, "ww");
  u64 before = ViewOf(&f)->cursor;
  Type(&f, "dw");
  Type(&f, "u");

  CHECK_STR(TextOf(&f), Str8Lit("one two three"));
  CHECK_EQ(ViewOf(&f)->cursor, before);

  Destroy(&f);
}

TEST(vim_undo_multiple_steps) {
  Fixture f = MakeFixture("a b c d");

  Type(&f, "dw");
  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("c d"));

  Type(&f, "uu");
  CHECK_STR(TextOf(&f), Str8Lit("a b c d"));

  Destroy(&f);
}

TEST(vim_repeat_last_change) {
  Fixture f = MakeFixture("one two three four");

  Type(&f, "dw");
  CHECK_STR(TextOf(&f), Str8Lit("two three four"));

  // `.` replays the chords that produced the change.
  Type(&f, ".");
  CHECK_STR(TextOf(&f), Str8Lit("three four"));
  Type(&f, ".");
  CHECK_STR(TextOf(&f), Str8Lit("four"));

  Destroy(&f);
}

TEST(vim_repeat_insert) {
  Fixture f = MakeFixture("ab");

  Type(&f, "iX<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("Xab"));

  Type(&f, "$.");
  CHECK_STR(TextOf(&f), Str8Lit("XaXb"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Windows, buffers and the command line
// ---------------------------------------------------------------------------

TEST(vim_window_splits_via_keybinding) {
  Fixture f = MakeFixture("hello");

  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 1);

  Type(&f, "<C-w>v");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Type(&f, "<C-w>s");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 3);

  Destroy(&f);
}

TEST(vim_split_shares_buffer_but_not_position) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  Type(&f, "<C-w>v");
  Panel *first = PanelFirstLeaf(f.ed.root_panel);
  Panel *second = PanelNextLeaf(f.ed.root_panel, first);

  // Both windows show the same buffer...
  CHECK(BufferHandleEqual(first->view->buffer, second->view->buffer));

  // ...but each keeps its own cursor.
  EditorFocusPanel(&f.ed, second);
  Type(&f, "jj");
  CHECK_EQ(ViewCursorLine(second->view, BufferOf(&f)), 2);
  CHECK_EQ(ViewCursorLine(first->view, BufferOf(&f)), 0);

  // An edit in one is visible in the other, since the text is shared.
  Type(&f, "dd");
  CHECK_STR(BufferTextAll(f.arena, BufferOf(&f)), Str8Lit("one\ntwo\n"));

  Destroy(&f);
}

TEST(vim_window_focus_movement) {
  Fixture f = MakeFixture("hello");

  Type(&f, "<C-w>v");
  Panel *left = PanelFirstLeaf(f.ed.root_panel);
  Panel *right = PanelNextLeaf(f.ed.root_panel, left);

  EditorFocusPanel(&f.ed, left);
  Type(&f, "<C-w>l");
  CHECK(f.ed.focused_panel == right);

  Type(&f, "<C-w>h");
  CHECK(f.ed.focused_panel == left);

  // Moving past the edge does nothing rather than wrapping.
  Type(&f, "<C-w>h");
  CHECK(f.ed.focused_panel == left);

  Destroy(&f);
}

TEST(vim_close_window) {
  Fixture f = MakeFixture("hello");

  Type(&f, "<C-w>v");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Type(&f, "<C-w>c");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 1);
  CHECK(!f.ed.quit);

  // Closing the last window is a request to exit.
  Type(&f, "<C-w>c");
  CHECK(f.ed.quit);

  Destroy(&f);
}

TEST(vim_leader_bindings) {
  Fixture f = MakeFixture("hello");

  // <leader> is space, matching the nvim config's mapleader.
  Type(&f, "<leader>v");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Type(&f, "<leader>h");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 3);

  Destroy(&f);
}

TEST(vim_ctrl_hjkl_moves_window_focus) {
  Fixture f = MakeFixture("hello");

  // The nvim config maps bare ctrl-hjkl alongside the built-in <C-w> forms.
  Type(&f, "<C-w>v");
  Panel *left = PanelFirstLeaf(f.ed.root_panel);
  Panel *right = PanelNextLeaf(f.ed.root_panel, left);

  EditorFocusPanel(&f.ed, left);
  Type(&f, "<C-l>");
  CHECK(f.ed.focused_panel == right);

  Type(&f, "<C-h>");
  CHECK(f.ed.focused_panel == left);

  // Both spellings still work.
  Type(&f, "<C-w>l");
  CHECK(f.ed.focused_panel == right);

  Destroy(&f);
}

TEST(vim_long_key_sequences_replay_in_full) {
  Fixture f = MakeFixture("");

  // Well past the length a single binding may have. Replayed input is a whole
  // session, so it must not be truncated -- or silently dropped, which is what
  // a bounded sequence would do.
  Type(&f, "iabcdefghijklmnopqrstuvwxyz0123456789<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("abcdefghijklmnopqrstuvwxyz0123456789"));

  Type(&f, "0dwdw");
  CHECK_EQ(BufferSize(BufferOf(&f)), 0);

  Destroy(&f);
}

TEST(vim_insert_mode_word_rubout) {
  Fixture f = MakeFixture("");

  Type(&f, "ihello world");
  Type(&f, "<C-w>");
  CHECK_STR(TextOf(&f), Str8Lit("hello "));

  // <C-h> is the nvim config's ctrl-backspace mapping, and <C-BS> is what a
  // window system actually sends for it.
  Type(&f, "again");
  Type(&f, "<C-h>");
  CHECK_STR(TextOf(&f), Str8Lit("hello "));
  Type(&f, "more");
  Type(&f, "<C-BS>");
  CHECK_STR(TextOf(&f), Str8Lit("hello "));

  // Rubbing out stops at the start of the line rather than eating the one above.
  Type(&f, "<C-w>");
  CHECK_STR(TextOf(&f), Str8Lit(""));

  Destroy(&f);
}

TEST(vim_partial_chord_waits_then_resolves) {
  Fixture f = MakeFixture("hello");

  // Halfway through <C-w>v the editor is waiting, having done nothing.
  EditorProcessChord(&f.ed, KeyChordKey(Key::W, KeyMod::Ctrl));
  CHECK(f.ed.input.pending != nullptr);
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 1);

  EditorProcessChord(&f.ed, KeyChordChar('v'));
  CHECK(f.ed.input.pending == nullptr);
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Destroy(&f);
}

TEST(vim_dead_end_chord_is_abandoned) {
  Fixture f = MakeFixture("hello world");

  // <C-w> followed by something unbound must not fall through and run whatever
  // that key would otherwise do.
  Type(&f, "<C-w>z");
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 1);
  CHECK(f.ed.input.pending == nullptr);
  CHECK_STR(TextOf(&f), Str8Lit("hello world"));

  // And the editor is still responsive afterwards.
  Type(&f, "w");
  CHECK_EQ(CursorColumn(&f), 6);

  Destroy(&f);
}

TEST(command_line_is_a_buffer) {
  Fixture f = MakeFixture("hello");

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  CHECK(command != nullptr);
  // An ordinary buffer that claims a few keys through a buffer-local keymap.
  // Nothing about it opts out of modal editing.
  CHECK_EQ((u32)command->kind, (u32)BufferKind::Command);
  CHECK(HasFlag(command->flags, BufferFlags::SingleLine));
  CHECK(command->hooks.keymap != nullptr);

  Destroy(&f);
}

TEST(command_line_runs_commands) {
  Fixture f = MakeFixture("hello");

  // The same command a keybinding would reach, arrived at by name.
  CHECK(CommandExecLine(&f.ed, Str8Lit("split-vertical")));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  // Vim's shorthand for the same thing.
  CHECK(CommandExecLine(&f.ed, Str8Lit("vs")));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 3);

  // An unknown name reports rather than doing something surprising.
  CHECK(!CommandExecLine(&f.ed, Str8Lit("nonsense")));
  CHECK(f.ed.status_message.size > 0);

  Destroy(&f);
}

TEST(command_line_quit_all) {
  Fixture f = MakeFixture("hello");

  CHECK(!f.ed.quit);
  CHECK(CommandExecLine(&f.ed, Str8Lit("qa")));
  CHECK(f.ed.quit);

  Destroy(&f);
}

TEST(vim_scrolling_commands) {
  Arena *tmp = ArenaAlloc(MB(4));
  String8List list = {};
  for (u64 i = 0; i < 200; i += 1) {
    Str8ListPush(tmp, &list, PushStr8F(tmp, "line %llu", (unsigned long long)i));
  }
  String8 text = Str8ListJoin(tmp, &list, Str8Lit("\n"));

  Fixture f = MakeFixture("");
  BufferSetText(&f.ed, BufferOf(&f), text);
  ViewSetCursor(ViewOf(&f), BufferOf(&f), 0);
  ArenaRelease(tmp);

  Type(&f, "<C-d>");
  CHECK(CursorLine(&f) > 0);

  u64 after_down = CursorLine(&f);
  Type(&f, "<C-u>");
  CHECK(CursorLine(&f) < after_down);

  // G scrolls the end of the file into view.
  Type(&f, "G");
  CHECK_EQ(CursorLine(&f), 199);
  CHECK(ViewIsCursorVisible(ViewOf(&f), BufferOf(&f), 80, 23));

  Destroy(&f);
}

TEST(vim_editing_long_file_keeps_line_index_correct) {
  Arena *tmp = ArenaAlloc(MB(4));
  String8List list = {};
  for (u64 i = 0; i < 500; i += 1) Str8ListPush(tmp, &list, Str8Lit("some line of text"));
  String8 text = Str8ListJoin(tmp, &list, Str8Lit("\n"));

  Fixture f = MakeFixture("");
  BufferSetText(&f.ed, BufferOf(&f), text);
  ViewSetCursor(ViewOf(&f), BufferOf(&f), 0);
  ArenaRelease(tmp);

  CHECK_EQ(BufferLineCount(BufferOf(&f)), 500);

  // A pile of edits scattered through the file, then a check that the line
  // index still agrees with a full rescan.
  for (u64 i = 0; i < 50; i += 1) {
    Type(&f, "dd");
    Type(&f, "j");
    Type(&f, "ohello<Esc>");
  }

  Buffer *buffer = BufferOf(&f);
  u64 expected = 1;
  u64 size = BufferSize(buffer);
  for (u64 p = 0; p < size; p += 1) {
    if (BufferByteAt(buffer, p) == '\n') expected += 1;
  }
  CHECK_EQ(BufferLineCount(buffer), expected);

  Destroy(&f);
}
