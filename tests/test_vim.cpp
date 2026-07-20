#include "editor/command.h"
#include "editor/editor.h"
#include "test.h"
#include "vim/vim_operators.h"

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

TEST(vim_word_motions_stop_on_empty_lines) {
  // Vim treats an empty line as a word. If `w` runs through one it lands on the
  // line after, and an operator over that motion drags the line break along.
  Fixture f = MakeFixture("alpha\n\n## Build");

  Type(&f, "w");
  CHECK_EQ(CursorLine(&f), 1);  // the blank line, not "## Build"

  Type(&f, "w");
  CHECK_EQ(CursorLine(&f), 2);

  // And the same going back.
  Type(&f, "b");
  CHECK_EQ(CursorLine(&f), 1);
  Type(&f, "b");
  CHECK_EQ(CursorLine(&f), 0);

  Destroy(&f);
}

TEST(vim_yank_before_empty_line_takes_only_the_word) {
  Fixture f = MakeFixture("alpha\n\n## Build");

  Type(&f, "yw");
  CHECK_STR(EditorGetRegister(&f.ed, 0).text, Str8Lit("alpha"));
  CHECK(!EditorGetRegister(&f.ed, 0).linewise);

  Destroy(&f);
}

TEST(vim_charwise_paste_stays_on_an_empty_line) {
  // On an empty line there is no character to paste "after", so p must not step
  // across the newline onto the following line. p and P coincide here.
  Fixture f = MakeFixture("alpha\n\n## Build");
  Type(&f, "ywjp");
  CHECK_STR(TextOf(&f), Str8Lit("alpha\nalpha\n## Build"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 3);
  Destroy(&f);

  Fixture g = MakeFixture("alpha\n\n## Build");
  Type(&g, "ywjP");
  CHECK_STR(TextOf(&g), Str8Lit("alpha\nalpha\n## Build"));
  CHECK_EQ(BufferLineCount(BufferOf(&g)), 3);
  Destroy(&g);

  // A non-empty line is unaffected: p still goes after the cursor.
  Fixture h = MakeFixture("ab\ncd");
  Type(&h, "ylp");
  CHECK_STR(TextOf(&h), Str8Lit("aab\ncd"));
  Destroy(&h);
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

TEST(vim_underscore_first_non_blank) {
  Fixture f = MakeFixture("    indented\n  second\n      third");

  // With no count `_` is where `^` is: the first non-blank of this line.
  Type(&f, "$_");
  CHECK_EQ(CursorLine(&f), 0);
  CHECK_EQ(CursorColumn(&f), 4);

  // A count moves count-1 lines down, landing on that line's first non-blank.
  Type(&f, "2_");
  CHECK_EQ(CursorLine(&f), 1);
  CHECK_EQ(CursorColumn(&f), 2);

  Type(&f, "gg3_");
  CHECK_EQ(CursorLine(&f), 2);
  CHECK_EQ(CursorColumn(&f), 6);

  // It clamps at the last line rather than failing.
  Type(&f, "gg99_");
  CHECK_EQ(CursorLine(&f), 2);

  Destroy(&f);
}

TEST(vim_underscore_is_linewise_unlike_caret) {
  // The difference that matters: `^` is exclusive, `_` is linewise, so the
  // same operator over each takes a very different span.
  Fixture caret = MakeFixture("  hello world\nsecond");
  Type(&caret, "$d^");
  // Charwise, back to the first non-blank, leaving the line in place.
  CHECK_STR(TextOf(&caret), Str8Lit("  d\nsecond"));
  CHECK_EQ(BufferLineCount(BufferOf(&caret)), 2);
  Destroy(&caret);

  Fixture underscore = MakeFixture("  hello world\nsecond");
  Type(&underscore, "$d_");
  // Linewise, so the whole line goes, exactly as dd would take it.
  CHECK_STR(TextOf(&underscore), Str8Lit("second"));
  CHECK_EQ(BufferLineCount(BufferOf(&underscore)), 1);
  Destroy(&underscore);

  // With a count it spans that many lines.
  Fixture counted = MakeFixture("a\nb\nc\nd");
  Type(&counted, "d2_");
  CHECK_STR(TextOf(&counted), Str8Lit("c\nd"));
  Destroy(&counted);
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

TEST(vim_exclusive_motion_stops_at_end_of_line) {
  // `w` from the last word of a line lands on the next line, but an operator
  // over it must stop at the line end rather than taking the break with it.
  Fixture f = MakeFixture("one two\nthree");

  Type(&f, "wdw");
  CHECK_STR(TextOf(&f), Str8Lit("one \nthree"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 2);

  // Yank behaves the same way, so pasting cannot introduce a line break that
  // was never yanked.
  Fixture g = MakeFixture("abc\ndef");
  Type(&g, "ywP");
  CHECK_STR(TextOf(&g), Str8Lit("abcabc\ndef"));
  CHECK_EQ(BufferLineCount(BufferOf(&g)), 2);
  Destroy(&g);

  // A motion that ends mid-line is untouched by the rule.
  Fixture h = MakeFixture("one two three");
  Type(&h, "dw");
  CHECK_STR(TextOf(&h), Str8Lit("two three"));
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

TEST(vim_typing_past_the_right_edge_scrolls) {
  Fixture f = MakeFixture("");

  i32 width = EditorPanelTextWidth(&f.ed, f.ed.focused_panel);
  CHECK(width > 0);

  Type(&f, "i");
  for (i32 i = 0; i < width + 20; i += 1) Type(&f, "x");

  // Typing moves the cursor without going through a command, so the scroll has
  // to happen on the text path too -- not only where CommandExec does it.
  View *view = ViewOf(&f);
  u64 column = CursorColumn(&f);
  CHECK(column > (u64)width);
  CHECK(view->scroll_column > 0);
  CHECK(column - view->scroll_column < (u64)width);

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

TEST(vim_visual_escape_restores_temporary_mouse_insert_mode) {
  Fixture insert = MakeFixture("hello");
  View *view = ViewOf(&insert);
  Buffer *buffer = BufferOf(&insert);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Insert;
  ViewSetCursor(view, buffer, 2);

  Type(&insert, "<Esc>");
  CHECK_EQ((u32)ModeOf(&insert), (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&insert), Str8Lit("hello"));
  Destroy(&insert);

  Fixture replace = MakeFixture("hello");
  view = ViewOf(&replace);
  buffer = BufferOf(&replace);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, 2);

  Type(&replace, "<Esc>");
  CHECK_EQ((u32)ModeOf(&replace), (u32)VimMode::Replace);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  Destroy(&replace);
}

TEST(vim_visual_operators_restore_temporary_mouse_modes) {
  Fixture yank = MakeFixture("alpha beta");
  View *view = ViewOf(&yank);
  Buffer *buffer = BufferOf(&yank);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, 4);

  Type(&yank, "y");
  CHECK_EQ((u32)ModeOf(&yank), (u32)VimMode::Replace);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&yank), Str8Lit("alpha beta"));
  Destroy(&yank);

  Fixture del = MakeFixture("alpha beta");
  view = ViewOf(&del);
  buffer = BufferOf(&del);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, 4);

  Type(&del, "d");
  CHECK_EQ((u32)ModeOf(&del), (u32)VimMode::Replace);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&del), Str8Lit(" beta"));
  Destroy(&del);

  Fixture indent = MakeFixture("one\ntwo");
  view = ViewOf(&indent);
  buffer = BufferOf(&indent);
  view->vim.mode = VimMode::VisualLine;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Insert;
  ViewSetCursor(view, buffer, BufferOffsetFromLine(buffer, 1));

  Type(&indent, ">");
  CHECK_EQ((u32)ModeOf(&indent), (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&indent), Str8Lit("  one\n  two"));
  Destroy(&indent);

  Fixture dedent = MakeFixture("  one\n  two");
  view = ViewOf(&dedent);
  buffer = BufferOf(&dedent);
  view->vim.mode = VimMode::VisualLine;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, BufferOffsetFromLine(buffer, 1));

  Type(&dedent, "<");
  CHECK_EQ((u32)ModeOf(&dedent), (u32)VimMode::Replace);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&dedent), Str8Lit("one\ntwo"));
  Destroy(&dedent);

  Fixture change = MakeFixture("alpha beta");
  view = ViewOf(&change);
  buffer = BufferOf(&change);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, 4);

  Type(&change, "c");
  CHECK_EQ((u32)ModeOf(&change), (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(TextOf(&change), Str8Lit(" beta"));
  Destroy(&change);
}

TEST(vim_replace_visual_charwise_captures_unnamed_and_returns_normal) {
  Fixture f = MakeFixture("abcdef");
  View *view = ViewOf(&f);
  Buffer *buffer = BufferOf(&f);

  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 1;
  ViewSetCursor(view, buffer, 3);

  u64 cursor = VimReplaceVisual(&f.ed, view, buffer, Register{Str8Lit("X"), false});

  CHECK_EQ(cursor, 1);
  CHECK_STR(TextOf(&f), Str8Lit("aXef"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);
  CHECK_EQ(view->cursor, 1);
  CHECK_STR(EditorGetRegister(&f.ed, 0).text, Str8Lit("bcd"));
  CHECK(!EditorGetRegister(&f.ed, 0).linewise);

  Destroy(&f);
}

TEST(vim_replace_visual_restores_temporary_insert_and_replace_modes) {
  Fixture insert = MakeFixture("abcdef");
  View *view = ViewOf(&insert);
  Buffer *buffer = BufferOf(&insert);

  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 1;
  view->vim.mouse_visual_return_mode = VimMode::Insert;
  ViewSetCursor(view, buffer, 3);

  u64 cursor = VimReplaceVisual(&insert.ed, view, buffer, Register{Str8Lit("XY"), false});

  CHECK_EQ(cursor, 1);
  CHECK_STR(TextOf(&insert), Str8Lit("aXYef"));
  CHECK_EQ((u32)ModeOf(&insert), (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_EQ(view->cursor, 1);
  Destroy(&insert);

  Fixture replace = MakeFixture("one\ntwo\nthree\n");
  view = ViewOf(&replace);
  buffer = BufferOf(&replace);

  view->vim.mode = VimMode::VisualLine;
  view->vim.visual_anchor = BufferOffsetFromLine(buffer, 1);
  view->vim.mouse_visual_return_mode = VimMode::Replace;
  ViewSetCursor(view, buffer, BufferOffsetFromLine(buffer, 1));

  cursor = VimReplaceVisual(&replace.ed, view, buffer, Register{Str8Lit("alpha\nbeta\n"), true});

  CHECK_EQ(cursor, BufferOffsetFromLine(buffer, 1));
  CHECK_STR(TextOf(&replace), Str8Lit("one\nalpha\nbeta\nthree\n"));
  CHECK_EQ((u32)ModeOf(&replace), (u32)VimMode::Replace);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_STR(EditorGetRegister(&replace.ed, 0).text, Str8Lit("two\n"));
  CHECK(EditorGetRegister(&replace.ed, 0).linewise);
  Destroy(&replace);
}

TEST(vim_replace_visual_linewise_normalizes_missing_final_newline) {
  Fixture f = MakeFixture("one\ntwo\nthree");
  View *view = ViewOf(&f);
  Buffer *buffer = BufferOf(&f);

  view->vim.mode = VimMode::VisualLine;
  view->vim.visual_anchor = BufferOffsetFromLine(buffer, 1);
  ViewSetCursor(view, buffer, BufferOffsetFromLine(buffer, 1));

  u64 cursor = VimReplaceVisual(&f.ed, view, buffer, Register{Str8Lit("tail"), true});

  CHECK_EQ(cursor, BufferOffsetFromLine(buffer, 1));
  CHECK_STR(TextOf(&f), Str8Lit("one\ntail\nthree"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);
  CHECK_STR(EditorGetRegister(&f.ed, 0).text, Str8Lit("two\n"));
  CHECK(EditorGetRegister(&f.ed, 0).linewise);

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
// Text objects
// ---------------------------------------------------------------------------

TEST(vim_text_object_word) {
  Fixture f = MakeFixture("foo(barbaz) end");

  // The point of a text object: it acts on the whole word regardless of where
  // in it the cursor happens to be, unlike `dw` which runs from the cursor.
  Type(&f, "lldiw");
  CHECK_STR(TextOf(&f), Str8Lit("(barbaz) end"));

  Fixture g = MakeFixture("alpha beta gamma");
  Type(&g, "wdaw");  // `aw` takes the trailing whitespace too
  CHECK_STR(TextOf(&g), Str8Lit("alpha gamma"));
  Destroy(&g);

  Destroy(&f);
}

TEST(vim_text_object_delimited_and_quoted) {
  Fixture f = MakeFixture("foo(barbaz) end");
  Type(&f, "f(lci(X<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("foo(X) end"));
  Destroy(&f);

  // `a(` includes the parentheses themselves.
  Fixture g = MakeFixture("foo(barbaz) end");
  Type(&g, "f(lda(");
  CHECK_STR(TextOf(&g), Str8Lit("foo end"));
  Destroy(&g);

  Fixture h = MakeFixture("say \"hello there\" ok");
  Type(&h, "f\"lci\"Q<Esc>");
  CHECK_STR(TextOf(&h), Str8Lit("say \"Q\" ok"));
  Destroy(&h);

  // Braces and brackets spell the same object.
  Fixture i = MakeFixture("x = {a, b};");
  Type(&i, "f{ldi{");
  CHECK_STR(TextOf(&i), Str8Lit("x = {};"));
  Destroy(&i);
}

TEST(vim_text_object_in_visual_mode) {
  Fixture f = MakeFixture("foo(barbaz) end");

  // In visual mode the object becomes the selection rather than being consumed.
  Type(&f, "f(lvi(");
  RangeU64 selection = ViewSelection(ViewOf(&f), BufferOf(&f));
  CHECK_EQ(selection.min, 4);
  CHECK_EQ(selection.max, 10);

  Type(&f, "d");
  CHECK_STR(TextOf(&f), Str8Lit("foo() end"));

  Destroy(&f);
}

TEST(vim_text_object_that_does_not_match_aborts) {
  Fixture f = MakeFixture("no parens here");

  // The cursor is not inside any pair, so the operator is abandoned rather than
  // acting on an empty range.
  Type(&f, "di(");
  CHECK_STR(TextOf(&f), Str8Lit("no parens here"));
  CHECK_EQ((u32)ModeOf(&f), (u32)VimMode::Normal);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

TEST(vim_macro_record_and_replay) {
  Fixture f = MakeFixture("one\ntwo\nthree");

  Type(&f, "qaA!<Esc>q");   // record "append a bang"
  CHECK_STR(TextOf(&f), Str8Lit("one!\ntwo\nthree"));

  Type(&f, "j@a");
  CHECK_STR(TextOf(&f), Str8Lit("one!\ntwo!\nthree"));

  // `@@` repeats the last macro; '@' is itself the register name meaning that.
  Type(&f, "j@@");
  CHECK_STR(TextOf(&f), Str8Lit("one!\ntwo!\nthree!"));

  Destroy(&f);
}

TEST(vim_macro_takes_a_count) {
  Fixture f = MakeFixture("abcdefgh");

  Type(&f, "qaxq");   // records a single delete, and performs one
  CHECK_STR(TextOf(&f), Str8Lit("bcdefgh"));

  Type(&f, "3@a");
  CHECK_STR(TextOf(&f), Str8Lit("efgh"));

  Destroy(&f);
}

TEST(vim_macro_is_stored_as_register_text) {
  Fixture f = MakeFixture("abc");

  Type(&f, "qbA!<Esc>q");
  // A macro is just text in a register, so it can be inspected, pasted and
  // edited like anything else that lands there.
  CHECK_STR(EditorGetRegister(&f.ed, 'b').text, Str8Lit("A!<Esc>"));

  Destroy(&f);
}

TEST(vim_macro_containing_an_edit_does_not_overwrite_itself) {
  Fixture f = MakeFixture("abcdefgh");

  // The delete inside the macro must not capture into the register holding the
  // macro -- that register is still selected while the macro runs.
  Type(&f, "qaxq");
  Type(&f, "@a");
  Type(&f, "@a");
  CHECK_STR(TextOf(&f), Str8Lit("defgh"));
  CHECK_STR(EditorGetRegister(&f.ed, 'a').text, Str8Lit("x"));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Registers and the system clipboard
//
// The clipboard reaches core through function pointers, so a fake one here
// exercises "+y and "+p with no window and no SDL.
// ---------------------------------------------------------------------------

namespace {

// Stands in for the system clipboard.
Arena *g_fake_clipboard_arena;
String8 g_fake_clipboard;
u32 g_fake_clipboard_writes;

String8 FakeClipboardRead(Arena *arena) { return PushStr8Copy(arena, g_fake_clipboard); }

void FakeClipboardWrite(String8 text) {
  ArenaClear(g_fake_clipboard_arena);
  g_fake_clipboard = PushStr8Copy(g_fake_clipboard_arena, text);
  g_fake_clipboard_writes += 1;
}

void InstallFakeClipboard(Fixture *f) {
  if (!g_fake_clipboard_arena) g_fake_clipboard_arena = ArenaAlloc(MB(4));
  ArenaClear(g_fake_clipboard_arena);
  g_fake_clipboard = String8{nullptr, 0};
  g_fake_clipboard_writes = 0;

  f->ed.clipboard.read = FakeClipboardRead;
  f->ed.clipboard.write = FakeClipboardWrite;
}

}  // namespace

TEST(vim_yank_to_system_clipboard) {
  Fixture f = MakeFixture("hello world");
  InstallFakeClipboard(&f);

  Type(&f, "\"+yw");
  CHECK_STR(g_fake_clipboard, Str8Lit("hello "));
  CHECK_EQ(g_fake_clipboard_writes, 1);

  // Vim fills the unnamed register alongside a named one, so a bare p still
  // pastes what was just yanked.
  Type(&f, "$p");
  CHECK_STR(TextOf(&f), Str8Lit("hello worldhello "));

  Destroy(&f);
}

TEST(vim_paste_from_system_clipboard) {
  Fixture f = MakeFixture("abc");
  InstallFakeClipboard(&f);

  // Something another program put on the clipboard.
  FakeClipboardWrite(Str8Lit("XYZ"));

  Type(&f, "\"+p");
  CHECK_STR(TextOf(&f), Str8Lit("aXYZbc"));

  // The internal register is untouched by a clipboard paste.
  Type(&f, "0ylp");
  CHECK_STR(TextOf(&f), Str8Lit("aaXYZbc"));

  Destroy(&f);
}

TEST(vim_clipboard_register_survives_an_operator) {
  Fixture f = MakeFixture("one two three");
  InstallFakeClipboard(&f);

  // The register selected by `"` has to live from there, through the operator,
  // to the motion that finally completes it.
  Type(&f, "\"+d2w");
  CHECK_STR(g_fake_clipboard, Str8Lit("one two "));
  CHECK_STR(TextOf(&f), Str8Lit("three"));

  Destroy(&f);
}

TEST(vim_named_registers) {
  Fixture f = MakeFixture("alpha beta");

  Type(&f, "\"ayw");   // "alpha " into register a
  Type(&f, "w\"byw");  // "beta" into register b

  CHECK_STR(EditorGetRegister(&f.ed, 'a').text, Str8Lit("alpha "));
  CHECK_STR(EditorGetRegister(&f.ed, 'b').text, Str8Lit("beta"));

  // Each pastes its own contents.
  Type(&f, "$\"ap");
  CHECK_STR(TextOf(&f), Str8Lit("alpha betaalpha "));

  Destroy(&f);
}

TEST(vim_register_selection_is_cleared_after_use) {
  Fixture f = MakeFixture("one two three");

  Type(&f, "\"ayw");
  CHECK_EQ(ViewOf(&f)->vim.pending_register, 0);  // consumed

  // A following yank with no `"` goes to the unnamed register, leaving a alone.
  Type(&f, "wyw");
  CHECK_STR(EditorGetRegister(&f.ed, 'a').text, Str8Lit("one "));
  CHECK_STR(EditorGetRegister(&f.ed, 0).text, Str8Lit("two "));

  Destroy(&f);
}

TEST(vim_yank_register_survives_deletes) {
  Fixture f = MakeFixture("alpha beta gamma");

  Type(&f, "yw");            // "0 and unnamed both hold "alpha "
  Type(&f, "wdw");           // a delete clobbers the unnamed register...

  CHECK_STR(EditorGetRegister(&f.ed, '0').text, Str8Lit("alpha "));
  CHECK_STR(EditorGetRegister(&f.ed, 0).text, Str8Lit("beta "));

  // ...so "0p is how the yank is still reachable, which is the whole point of
  // vim keeping them apart.
  Type(&f, "$\"0p");
  CHECK_STR(TextOf(&f), Str8Lit("alpha gammaalpha "));

  Destroy(&f);
}

TEST(vim_quote_names_the_unnamed_register) {
  Fixture f = MakeFixture("abc def");

  Type(&f, "yw");
  // `""` is how vim spells the unnamed register, so it must reach the same
  // place a bare p does.
  Type(&f, "$\"\"p");
  CHECK_STR(TextOf(&f), Str8Lit("abc defabc "));

  Destroy(&f);
}

TEST(vim_insert_register_with_ctrl_r) {
  Fixture f = MakeFixture("abc");
  InstallFakeClipboard(&f);
  FakeClipboardWrite(Str8Lit("PASTED"));

  // <C-r>{reg} inserts without leaving insert mode.
  Type(&f, "A <C-r>+!<Esc>");
  CHECK_STR(TextOf(&f), Str8Lit("abc PASTED!"));

  Destroy(&f);
}

TEST(vim_clipboard_paste_is_charwise_for_ordinary_text) {
  Fixture f = MakeFixture("abc");
  InstallFakeClipboard(&f);

  // Copied from a browser or a terminal, where a trailing newline usually
  // comes along with the selection and is not meant as "this is a whole line".
  FakeClipboardWrite(Str8Lit("XYZ\n"));

  Type(&f, "\"+p");
  // Should land inside the line, not push a new one.
  CHECK_STR(TextOf(&f), Str8Lit("aXYZbc"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 1);

  Destroy(&f);
}

TEST(vim_clipboard_paste_is_linewise_for_multiple_lines) {
  Fixture f = MakeFixture("one\ntwo");
  InstallFakeClipboard(&f);

  // Genuinely several lines, so linewise is what was meant.
  FakeClipboardWrite(Str8Lit("alpha\nbeta\n"));

  Type(&f, "\"+p");
  CHECK_STR(TextOf(&f), Str8Lit("one\nalpha\nbeta\ntwo"));

  Destroy(&f);
}

TEST(vim_clipboard_round_trip_keeps_its_kind) {
  Fixture f = MakeFixture("one\ntwo\nthree");
  InstallFakeClipboard(&f);

  // A linewise yank of ours must come back linewise, even though the text is a
  // single line with a trailing newline -- which an external copy would not be.
  Type(&f, "\"+yy");
  Type(&f, "j\"+p");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\none\nthree"));

  // And a charwise yank of ours stays charwise.
  Fixture g = MakeFixture("abc def");
  InstallFakeClipboard(&g);
  Type(&g, "\"+yw");
  Type(&g, "$\"+p");
  CHECK_STR(TextOf(&g), Str8Lit("abc defabc "));
  CHECK_EQ(BufferLineCount(BufferOf(&g)), 1);
  Destroy(&g);

  Destroy(&f);
}

TEST(vim_clipboard_aliases_share_linewise_metadata) {
  Fixture f = MakeFixture("one\ntwo");
  InstallFakeClipboard(&f);

  Type(&f, "\"+yy");
  Register star = EditorGetRegister(&f.ed, '*');
  CHECK_STR(star.text, Str8Lit("one\n"));
  CHECK(star.linewise);

  Destroy(&f);
}

TEST(vim_paste_before_with_clipboard) {
  Fixture f = MakeFixture("abc");
  InstallFakeClipboard(&f);
  FakeClipboardWrite(Str8Lit("XYZ\n"));

  // P must behave the same way: inline, before the cursor.
  Type(&f, "\"+P");
  CHECK_STR(TextOf(&f), Str8Lit("XYZabc"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 1);

  Destroy(&f);
}

TEST(vim_linewise_paste_of_last_line_without_trailing_newline) {
  // `yy` on a final line with no newline of its own yields register text that
  // does not end in one; pasting it must still make a whole line rather than
  // running into the neighbouring one.
  Fixture f = MakeFixture("one\ntwo");

  Type(&f, "jyy");
  Type(&f, "P");
  CHECK_STR(TextOf(&f), Str8Lit("one\ntwo\ntwo"));
  CHECK_EQ(BufferLineCount(BufferOf(&f)), 3);

  Destroy(&f);
}

TEST(vim_clipboard_absent_is_harmless) {
  Fixture f = MakeFixture("abc");
  // No hooks installed, as would be the case in a headless build.
  f.ed.clipboard.read = nullptr;
  f.ed.clipboard.write = nullptr;

  Type(&f, "\"+yy");
  Type(&f, "\"+p");
  CHECK_STR(TextOf(&f), Str8Lit("abc"));  // nothing happens, nothing crashes

  Destroy(&f);
}

TEST(vim_zoom_adjusts_font_size) {
  Fixture f = MakeFixture("abc");

  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeDefault);

  Type(&f, "<C-=>");
  CHECK(f.ed.font_size > kFontSizeDefault);
  // The app watches this to know it must rebuild the atlas.
  CHECK(f.ed.font_size_changed);

  f.ed.font_size_changed = false;
  Type(&f, "<C-->");
  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeDefault);
  CHECK(f.ed.font_size_changed);

  // A count zooms by that many steps, and reset returns to the default.
  Type(&f, "5<C-=>");
  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeDefault + 5);
  Type(&f, "<C-0>");
  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeDefault);

  // It clamps rather than running away.
  for (u32 i = 0; i < 200; i += 1) Type(&f, "<C-->");
  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeMin);
  for (u32 i = 0; i < 400; i += 1) Type(&f, "<C-=>");
  CHECK_EQ((i64)f.ed.font_size, (i64)kFontSizeMax);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Line numbers
// ---------------------------------------------------------------------------

TEST(line_numbers_relative_labels_count_from_the_cursor) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour\nfive");
  View *view = ViewOf(&f);
  Buffer *buffer = BufferOf(&f);

  Type(&f, "2j");
  CHECK_EQ(CursorLine(&f), 2);

  // Zero on the cursor's own line, distances either side of it.
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 2), 0);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 1), 1);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 0), 2);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 3), 1);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 4), 2);

  // They follow the cursor rather than being computed once.
  Type(&f, "j");
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 2), 1);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 3), 0);

  Destroy(&f);
}

TEST(line_numbers_absolute_labels_are_one_based) {
  Fixture f = MakeFixture("one\ntwo\nthree");
  f.ed.line_number_mode = LineNumberMode::Absolute;

  View *view = ViewOf(&f);
  Buffer *buffer = BufferOf(&f);

  Type(&f, "j");
  // The cursor makes no difference here, unlike the relative case above.
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 0), 1);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 1), 2);
  CHECK_EQ(EditorLineNumberLabel(&f.ed, view, buffer, 2), 3);

  Destroy(&f);
}

TEST(line_numbers_gutter_width_follows_the_line_count) {
  Fixture f = MakeFixture("a\nb\nc");
  Panel *panel = f.ed.focused_panel;

  // A short buffer still gets the minimum, so the text does not sit flush
  // against the edge.
  CHECK_EQ(EditorGutterWidth(&f.ed, panel), kLineNumberMinDigits + 1);

  // A buffer past the minimum widens by a column per digit.
  const u64 count = 1000;
  u8 *data = PushArrayNoZero(f.arena, u8, count * 2);
  for (u64 i = 0; i < count; i += 1) {
    data[i * 2] = 'x';
    data[i * 2 + 1] = '\n';
  }
  BufferSetText(&f.ed, BufferOf(&f), String8{data, count * 2 - 1});
  CHECK_EQ(BufferLineCount(BufferOf(&f)), count);
  CHECK_EQ(EditorGutterWidth(&f.ed, panel), 5);  // 4 digits + the blank column

  f.ed.line_number_mode = LineNumberMode::Off;
  CHECK_EQ(EditorGutterWidth(&f.ed, panel), 0);

  Destroy(&f);
}

TEST(line_numbers_narrow_the_text_area) {
  Fixture f = MakeFixture("a\nb\nc");
  Panel *panel = f.ed.focused_panel;

  i32 panel_width = RectWidth(panel->rect);
  i32 gutter = EditorGutterWidth(&f.ed, panel);
  CHECK(gutter > 0);

  // The gutter comes off the text rect, not off the panel: horizontal scrolling
  // reads this width, so a gutter applied only in the renderer would scroll the
  // cursor off the right edge.
  CHECK_EQ(EditorPanelTextWidth(&f.ed, panel), panel_width - gutter);
  CHECK_EQ(RectWidth(panel->rect), panel_width);

  f.ed.line_number_mode = LineNumberMode::Off;
  CHECK_EQ(EditorPanelTextWidth(&f.ed, panel), panel_width);

  Destroy(&f);
}

TEST(line_numbers_commands_switch_the_mode) {
  Fixture f = MakeFixture("a\nb");

  CHECK(f.ed.line_number_mode == kLineNumberModeDefault);

  CHECK(CommandExecLine(&f.ed, Str8Lit("number")));
  CHECK(f.ed.line_number_mode == LineNumberMode::Absolute);

  CHECK(CommandExecLine(&f.ed, Str8Lit("nonumber")));
  CHECK(f.ed.line_number_mode == LineNumberMode::Off);

  CHECK(CommandExecLine(&f.ed, Str8Lit("relativenumber")));
  CHECK(f.ed.line_number_mode == LineNumberMode::Relative);

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

// ---------------------------------------------------------------------------
// Jump list (<C-o> / <C-i>)
// ---------------------------------------------------------------------------

TEST(vim_jump_list_back_and_forth) {
  Fixture f = MakeFixture("one\ntwo\nthree\nfour");

  Type(&f, "G");
  CHECK_EQ(CursorLine(&f), 3);
  Type(&f, "<C-o>");
  CHECK_EQ(CursorLine(&f), 0);
  Type(&f, "<C-i>");
  CHECK_EQ(CursorLine(&f), 3);
  // Tab is the same command as <C-i> on this input path.
  Type(&f, "<C-o>");
  Type(&f, "<Tab>");
  CHECK_EQ(CursorLine(&f), 3);

  Destroy(&f);
}

TEST(vim_jump_list_records_jumps_not_every_motion) {
  Fixture f = MakeFixture("aaa\n\nbbb\n\n(ccc)");

  Type(&f, "jj");  // onto "bbb" via plain motions -- not a jump
  u64 mid = ViewOf(&f)->cursor;
  Type(&f, "G");
  Type(&f, "<C-o>");
  // G recorded the position it left, which is where j/j landed.
  CHECK_EQ(ViewOf(&f)->cursor, mid);

  // Further plain motions do not create entries: after gg, <C-o> returns to mid
  // rather than to wherever j/w left the cursor.
  Type(&f, "gg");
  Type(&f, "wll");
  Type(&f, "<C-o>");
  CHECK_EQ(ViewOf(&f)->cursor, mid);

  // Paragraph motion is a jump.
  Fixture para = MakeFixture("aaa\n\nbbb\n\nccc");
  Type(&para, "}");
  CHECK(CursorLine(&para) > 0);
  u64 para_line = CursorLine(&para);
  Type(&para, "<C-o>");
  CHECK_EQ(CursorLine(&para), 0);
  Type(&para, "<C-i>");
  CHECK_EQ(CursorLine(&para), para_line);
  Destroy(&para);

  // % is a jump.
  Fixture bracket = MakeFixture("(abc)");
  Type(&bracket, "%");
  CHECK_EQ(CursorColumn(&bracket), 4);
  Type(&bracket, "<C-o>");
  CHECK_EQ(CursorColumn(&bracket), 0);
  Destroy(&bracket);

  Destroy(&f);
}

TEST(vim_jump_list_search_and_count) {
  Fixture f = MakeFixture("alpha\nbeta\nalpha\ngamma\nalpha");

  // `/` starts after the cursor, so the first hit is the second "alpha".
  Type(&f, "/alpha<CR>");
  CHECK_EQ(CursorLine(&f), 2);
  Type(&f, "n");
  CHECK_EQ(CursorLine(&f), 4);
  Type(&f, "2<C-o>");
  CHECK_EQ(CursorLine(&f), 0);

  Destroy(&f);
}

TEST(vim_jump_list_truncates_forward_on_new_jump) {
  Fixture f = MakeFixture("a\nb\nc\nd\ne");

  Type(&f, "G");
  Type(&f, "gg");
  Type(&f, "3G");
  CHECK_EQ(CursorLine(&f), 2);
  Type(&f, "<C-o>");
  CHECK_EQ(CursorLine(&f), 0);
  // A new jump from here drops the forward half (the old G destination).
  Type(&f, "2G");
  CHECK_EQ(CursorLine(&f), 1);
  Type(&f, "<C-i>");
  CHECK_EQ(CursorLine(&f), 1);

  Destroy(&f);
}

TEST(vim_jump_list_across_buffers) {
  Fixture f = MakeFixture("first\nsecond\nthird");

  Type(&f, "j");
  CHECK_EQ(CursorLine(&f), 1);
  BufferHandle first = ViewOf(&f)->buffer;

  BufferHandle other = BufferOpen(&f.ed.buffers, BufferKind::Scratch, Str8Lit("other"));
  Buffer *other_buf = BufferFromHandle(&f.ed.buffers, other);
  BufferSetText(&f.ed, other_buf, Str8Lit("other line\n"));

  EditorPushJump(&f.ed, ViewOf(&f));
  EditorShowBuffer(&f.ed, other);
  CHECK(BufferHandleEqual(ViewOf(&f)->buffer, other));

  Type(&f, "<C-o>");
  CHECK(BufferHandleEqual(ViewOf(&f)->buffer, first));
  CHECK_EQ(CursorLine(&f), 1);

  Destroy(&f);
}
