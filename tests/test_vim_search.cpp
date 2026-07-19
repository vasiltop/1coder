#include "editor/command.h"
#include "editor/editor.h"
#include "test.h"
#include "vim/vim_search.h"

// In-file search, driven through the keyboard exactly as `/` is used, so the
// prompt buffer, the incremental preview and the commit path are all covered.

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
View *ViewOf(Fixture *f) { return EditorFocusedView(&f->ed); }
Buffer *BufferOf(Fixture *f) { return EditorFocusedBuffer(&f->ed); }
u64 CursorLine(Fixture *f) { return ViewCursorLine(ViewOf(f), BufferOf(f)); }
u64 CursorColumn(Fixture *f) { return ViewCursorColumn(ViewOf(f), BufferOf(f)); }

const char *kSample =
    "alpha beta\n"
    "gamma delta\n"
    "beta epsilon\n"
    "Beta zeta";

}  // namespace

// ---------------------------------------------------------------------------
// The pure search itself
// ---------------------------------------------------------------------------

TEST(search_finds_next_occurrence) {
  Fixture f = MakeFixture(kSample);

  SearchHit hit = BufferSearch(BufferOf(&f), Str8Lit("beta"), 0, true, true);
  CHECK(hit.found);
  CHECK(!hit.wrapped);
  CHECK_EQ(hit.offset, (u64)6);  // "alpha beta"

  Destroy(&f);
}

// A search starting on top of a match must move off it, or `n` would never
// advance.
TEST(search_skips_the_match_it_starts_on) {
  Fixture f = MakeFixture(kSample);

  SearchHit hit = BufferSearch(BufferOf(&f), Str8Lit("beta"), 6, true, true);
  CHECK(hit.found);
  CHECK_EQ(hit.offset, (u64)23);  // start of line 3

  Destroy(&f);
}

TEST(search_wraps_and_reports_it) {
  Fixture f = MakeFixture(kSample);

  // Past the last match, so the only way to find one is round the end.
  SearchHit hit = BufferSearch(BufferOf(&f), Str8Lit("alpha"), 40, true, true);
  CHECK(hit.found);
  CHECK(hit.wrapped);
  CHECK_EQ(hit.offset, (u64)0);

  // Without wrapping, the same search fails.
  SearchHit nowrap = BufferSearch(BufferOf(&f), Str8Lit("alpha"), 40, true, false);
  CHECK(!nowrap.found);

  Destroy(&f);
}

TEST(search_backward_finds_the_nearest_match_behind) {
  Fixture f = MakeFixture(kSample);

  SearchHit hit = BufferSearch(BufferOf(&f), Str8Lit("beta"), 30, false, true);
  CHECK(hit.found);
  CHECK_EQ(hit.offset, (u64)23);

  Destroy(&f);
}

// Smartcase: a lowercase pattern matches either case, an uppercase one is
// taken literally.
TEST(search_is_smartcase) {
  Fixture f = MakeFixture(kSample);

  CHECK(!SearchPatternIsCaseSensitive(Str8Lit("beta")));
  CHECK(SearchPatternIsCaseSensitive(Str8Lit("Beta")));

  // "Beta" on line 4 only, since the pattern carries a capital.
  SearchHit exact = BufferSearch(BufferOf(&f), Str8Lit("Beta"), 0, true, true);
  CHECK(exact.found);
  CHECK_EQ(exact.offset, (u64)36);

  // Lowercase reaches the capitalised one too.
  SearchHit loose = BufferSearch(BufferOf(&f), Str8Lit("beta"), 30, true, true);
  CHECK(loose.found);
  CHECK_EQ(loose.offset, (u64)36);

  Destroy(&f);
}

TEST(search_all_collects_matches_in_a_range) {
  Fixture f = MakeFixture(kSample);

  RangeU64 hits[8];
  u64 count = BufferSearchAll(BufferOf(&f), Str8Lit("beta"), RangeU64{0, 64}, hits, 8);
  CHECK_EQ(count, (u64)3);  // two lowercase plus the capitalised one
  CHECK_EQ(hits[0].min, (u64)6);
  CHECK_EQ(hits[0].max, (u64)10);

  Destroy(&f);
}

TEST(word_at_cursor_spans_the_whole_word) {
  Fixture f = MakeFixture(kSample);
  Arena *arena = ArenaAlloc(MB(1));

  // From the middle of "alpha".
  CHECK_STR(BufferWordAtCursor(arena, BufferOf(&f), 2), Str8Lit("alpha"));
  // From the space before "beta", vim scans forward to the next word.
  CHECK_STR(BufferWordAtCursor(arena, BufferOf(&f), 5), Str8Lit("beta"));

  ArenaRelease(arena);
  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Driven from the keyboard
// ---------------------------------------------------------------------------

TEST(slash_opens_a_prompt_and_jumps_on_submit) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/");
  CHECK(f.ed.command_line_active);
  CHECK_EQ((u64)f.ed.command_line_prompt, (u64)'/');

  Type(&f, "delta<CR>");
  CHECK(!f.ed.command_line_active);
  CHECK_EQ(CursorLine(&f), (u64)1);
  CHECK_EQ(CursorColumn(&f), (u64)6);

  Destroy(&f);
}

// The prompt is an ordinary vim buffer, so editing the pattern works. This is
// the property that "everything is a buffer" is supposed to buy.
TEST(search_prompt_is_editable_with_vim_bindings) {
  Fixture f = MakeFixture(kSample);

  // Type a wrong pattern, drop to normal mode, replace the line, retype.
  Type(&f, "/WRONG<Esc>");
  Type(&f, "ccepsilon");  // operators and insert work on the prompt buffer
  Type(&f, "<CR>");

  CHECK_EQ(CursorLine(&f), (u64)2);

  Destroy(&f);
}

// Typing moves the cursor to the prospective match before Enter is pressed.
TEST(search_previews_incrementally) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/gam");
  CHECK(f.ed.command_line_active);
  CHECK_EQ(CursorLine(&f), (u64)1);  // already showing the match

  Destroy(&f);
}

// And abandoning the search puts the cursor back where `/` was pressed.
TEST(cancelling_a_search_restores_the_cursor) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "j");  // start on line 1
  u64 origin = ViewOf(&f)->cursor;

  Type(&f, "/epsilon");
  CHECK_EQ(CursorLine(&f), (u64)2);

  Type(&f, "<Esc><Esc>");  // first Esc leaves insert, second abandons
  CHECK(!f.ed.command_line_active);
  CHECK_EQ(ViewOf(&f)->cursor, origin);
  CHECK(!f.ed.search_highlight);

  Destroy(&f);
}

TEST(n_and_N_repeat_the_search) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/beta<CR>");
  CHECK_EQ(CursorLine(&f), (u64)0);

  Type(&f, "n");
  CHECK_EQ(CursorLine(&f), (u64)2);
  Type(&f, "n");
  CHECK_EQ(CursorLine(&f), (u64)3);  // matches "Beta" -- lowercase is loose

  Type(&f, "N");
  CHECK_EQ(CursorLine(&f), (u64)2);

  Destroy(&f);
}

TEST(n_counts) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/beta<CR>");
  Type(&f, "2n");
  CHECK_EQ(CursorLine(&f), (u64)3);

  Destroy(&f);
}

// `?` searches backward, and `n` then follows that direction rather than
// always going forward.
TEST(question_mark_searches_backward) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "G");  // last line -- on "Beta", which the search must skip
  Type(&f, "?beta<CR>");
  CHECK_EQ(CursorLine(&f), (u64)2);

  Type(&f, "n");
  CHECK_EQ(CursorLine(&f), (u64)0);

  Destroy(&f);
}

TEST(star_searches_for_the_word_under_the_cursor) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "w");  // onto "beta" on line 0
  Type(&f, "*");
  CHECK_EQ(CursorLine(&f), (u64)2);

  Destroy(&f);
}

// From the middle of a word, `*` must not match the word it started on.
TEST(star_from_mid_word_skips_its_own_word) {
  Fixture f = MakeFixture("hello world\nhello again\n");

  Type(&f, "ll");  // into the middle of "hello"
  Type(&f, "*");
  CHECK_EQ(CursorLine(&f), (u64)1);
  CHECK_EQ(CursorColumn(&f), (u64)0);

  Destroy(&f);
}

TEST(hash_searches_backward_for_the_word) {
  Fixture f = MakeFixture("hello world\nhello again\n");

  Type(&f, "j");
  Type(&f, "#");
  CHECK_EQ(CursorLine(&f), (u64)0);

  Destroy(&f);
}

TEST(search_sets_the_highlight_and_nohlsearch_clears_it) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/beta<CR>");
  CHECK(f.ed.search_highlight);
  CHECK_STR(f.ed.search_pattern, Str8Lit("beta"));

  Type(&f, "<leader>/");
  CHECK(!f.ed.search_highlight);
  // The pattern survives, so `n` still works after clearing the highlight.
  CHECK_STR(f.ed.search_pattern, Str8Lit("beta"));

  Type(&f, "n");
  CHECK(f.ed.search_highlight);

  Destroy(&f);
}

// An empty pattern repeats the last one, as in vim.
TEST(empty_search_repeats_the_previous_pattern) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "/beta<CR>");
  CHECK_EQ(CursorLine(&f), (u64)0);

  Type(&f, "/<CR>");
  CHECK_EQ(CursorLine(&f), (u64)2);

  Destroy(&f);
}

TEST(search_from_the_command_line_by_name) {
  Fixture f = MakeFixture(kSample);

  CHECK(CommandExecLine(&f.ed, Str8Lit("search delta")));
  CHECK_EQ(CursorLine(&f), (u64)1);

  Destroy(&f);
}

TEST(failed_search_leaves_the_cursor_alone) {
  Fixture f = MakeFixture(kSample);

  Type(&f, "j");
  u64 before = ViewOf(&f)->cursor;

  Type(&f, "/nothinghere<CR>");
  CHECK_EQ(ViewOf(&f)->cursor, before);

  Destroy(&f);
}
