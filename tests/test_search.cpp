#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"
#include "search/search.h"
#include "test.h"
#include "test_tempdir.h"

namespace {

// A small tree on disk, since walking and grepping are about the filesystem.
// `arena` and `root` are borrowed from `dir`, which owns both.
struct Tree {
  TempDir dir;
  Arena *arena;
  String8 root;
};

Tree MakeTree(const char *tag) {
  Tree tree = {};
  tree.dir = MakeTempDir(tag);
  tree.arena = tree.dir.arena;
  tree.root = tree.dir.path;

  // .git and build are here to be skipped by the walk, and binary.o to be
  // skipped by the grep.
  const char *dirs[] = {"src/deep", ".git", "build"};
  for (const char *sub : dirs) {
    bool ok = OsMakeDirs(TempPath(&tree.dir, sub));
    CHECK(ok);
  }

  struct Fixture {
    const char *path;
    String8 contents;
  } files[] = {
      {"src/one.cpp", Str8Lit("alpha needle\nbeta\n")},
      {"src/deep/two.cpp", Str8Lit("gamma\nNEEDLE here\n")},
      {"src/three.txt", Str8Lit("nothing\n")},
      {".git/hidden.cpp", Str8Lit("needle\n")},
      {"build/generated.cpp", Str8Lit("needle\n")},
      {"binary.o", Str8Lit("needle\n")},
  };
  for (const Fixture &file : files) {
    bool ok = OsFileWrite(TempPath(&tree.dir, file.path), file.contents);
    CHECK(ok);
  }

  return tree;
}

// Qualified, or unqualified lookup stops at this overload and never finds the
// TempDir one at global scope.
void Destroy(Tree *tree) { ::Destroy(&tree->dir); }

bool ListContains(PathList list, const char *want) {
  for (u64 i = 0; i < list.count; i += 1) {
    if (Str8Match(list.paths[i], Str8C(want))) return true;
  }
  return false;
}

}  // namespace

TEST(search_walk_finds_files_and_skips_noise) {
  Tree tree = MakeTree("walk");
  PathList files = SearchWalkFiles(tree.arena, tree.root);

  // Nested files are found, with paths relative to the root.
  CHECK(ListContains(files, "src/one.cpp"));
  CHECK(ListContains(files, "src/deep/two.cpp"));
  CHECK(ListContains(files, "src/three.txt"));

  // Version control, build output and object files are not worth walking.
  CHECK(!ListContains(files, ".git/hidden.cpp"));
  CHECK(!ListContains(files, "build/generated.cpp"));
  CHECK(!ListContains(files, "binary.o"));

  Destroy(&tree);
}

TEST(search_walk_survives_a_deep_tree) {
  // The list being accumulated must outlive each directory's own scratch, or
  // recursing into a subdirectory would discard everything found so far.
  Tree tree = MakeTree("deep");

  CHECK(OsMakeDirs(TempPath(&tree.dir, "a/b/c/d/e")));
  for (int i = 1; i <= 5; i += 1) {
    TempArena scratch = ScratchBegin1(tree.arena);
    String8 path = PushStr8F(scratch.arena, "a/b/c/d/e/f%d.cpp", i);
    CHECK(OsFileWrite(OsPathJoin(scratch.arena, tree.root, path), Str8Lit("x\n")));
    ScratchEnd(scratch);
  }

  PathList files = SearchWalkFiles(tree.arena, tree.root);
  CHECK(ListContains(files, "a/b/c/d/e/f1.cpp"));
  CHECK(ListContains(files, "a/b/c/d/e/f5.cpp"));
  CHECK(ListContains(files, "src/one.cpp"));  // still there after the recursion

  Destroy(&tree);
}

TEST(search_grep_finds_matches_with_smartcase) {
  Tree tree = MakeTree("grep");

  // A lowercase pattern matches either case, as vim's 'smartcase' does.
  GrepResults lower = SearchGrep(tree.arena, tree.root, Str8Lit("needle"));
  CHECK_EQ(lower.count, 2);

  // An uppercase one is taken literally.
  GrepResults upper = SearchGrep(tree.arena, tree.root, Str8Lit("NEEDLE"));
  CHECK_EQ(upper.count, 1);
  CHECK_EQ(upper.matches[0].line, 2);
  CHECK_STR(upper.matches[0].text, Str8Lit("NEEDLE here"));

  // Skipped directories stay skipped when grepping, not just when listing.
  CHECK_EQ(SearchGrep(tree.arena, tree.root, Str8Lit("nothing")).count, 1);
  CHECK_EQ(SearchGrep(tree.arena, tree.root, Str8Lit("no-such-text")).count, 0);
  CHECK_EQ(SearchGrep(tree.arena, tree.root, Str8Lit("")).count, 0);

  Destroy(&tree);
}

TEST(grep_without_a_pattern_prompts) {
  Tree tree = MakeTree("prompt");

  Arena *arena = ArenaAlloc(MB(64));
  Editor ed = {};
  EditorInit(&ed, arena, RectS32{0, 0, 80, 25});
  ed.cwd = PushStr8Copy(arena, tree.root);

  // A binding carries no argument text. Searching for nothing and reporting no
  // matches would look like a broken command, so it asks for a pattern instead.
  CommandExec(&ed, CommandId::grep);

  CHECK(ed.command_line_active);
  Buffer *command = BufferFromHandle(&ed.buffers, ed.command_buffer);
  CHECK_STR(BufferTextAll(arena, command), Str8Lit("grep "));
  CHECK_EQ((u32)ed.command_view->vim.mode, (u32)VimMode::Insert);
  CHECK_EQ(ed.command_view->cursor, 5);  // waiting at the end, ready to type

  EditorProcessSpec(&ed, "needle<CR>");
  CHECK(!ed.command_line_active);
  CHECK_STR(EditorFocusedBuffer(&ed)->name, Str8Lit("[grep]"));

  EditorDestroy(&ed);
  ArenaRelease(arena);
  Destroy(&tree);
}

TEST(live_grep_searches_as_you_type) {
  Tree tree = MakeTree("live");

  Arena *arena = ArenaAlloc(MB(64));
  Editor ed = {};
  EditorInit(&ed, arena, RectS32{0, 0, 80, 25});
  ed.cwd = PushStr8Copy(arena, tree.root);

  // <leader>pg opens it directly -- no pattern to supply up front, because the
  // search reruns on every keystroke.
  EditorProcessSpec(&ed, "<leader>pg");

  Buffer *live = EditorFocusedBuffer(&ed);
  CHECK_STR(live->name, Str8Lit("[live-grep]"));
  CHECK_EQ((u32)EditorFocusedView(&ed)->vim.mode, (u32)VimMode::Insert);
  CHECK(live->hooks.on_edit != nullptr);

  // An empty query matches nothing, so the buffer is just the query line.
  CHECK_EQ(BufferLineCount(live), 1);

  EditorProcessSpec(&ed, "needle");
  CHECK_STR(BufferLineText(arena, live, 0), Str8Lit("needle"));
  // Two files contain it, and smartcase means the lowercase query finds both.
  CHECK_EQ(BufferLineCount(live), 3);

  // Narrowing the query narrows the results, without disturbing what was typed.
  // Smartcase still applies, so this lowercase query finds "NEEDLE here".
  EditorProcessSpec(&ed, " here");
  CHECK_STR(BufferLineText(arena, live, 0), Str8Lit("needle here"));
  CHECK_EQ(BufferLineCount(live), 2);

  // Narrowing to something absent leaves the query line alone.
  EditorProcessSpec(&ed, "zzz");
  CHECK_EQ(BufferLineCount(live), 1);

  // Backspacing widens them again, which is the whole point of it being live.
  EditorProcessSpec(&ed, "<BS><BS><BS><BS><BS><BS><BS><BS>");
  CHECK_STR(BufferLineText(arena, live, 0), Str8Lit("needle"));
  CHECK_EQ(BufferLineCount(live), 3);

  // Enter while still typing on the query line takes the first match, so a
  // search can be run and followed without leaving insert mode.
  String8 first = BufferLineText(arena, live, 1);
  u64 colon = Str8FindFirstChar(first, ':');
  String8 first_path = Str8Prefix(first, colon);
  u64 first_line = 0;
  for (u64 i = colon + 1; i < first.size && CharIsDigit(first.str[i]); i += 1) {
    first_line = first_line * 10 + (u64)(first.str[i] - '0');
  }

  CHECK_EQ(ViewCursorLine(EditorFocusedView(&ed), live), 0);  // still on the query
  EditorProcessSpec(&ed, "<CR>");
  CHECK(Str8EndsWith(EditorFocusedBuffer(&ed)->path, first_path));
  CHECK_EQ(ViewCursorLine(EditorFocusedView(&ed), EditorFocusedBuffer(&ed)), first_line - 1);

  EditorDestroy(&ed);
  ArenaRelease(arena);
  Destroy(&tree);
}

TEST(fuzzy_score_ranks_sensibly) {
  // A subsequence matches; anything else does not.
  CHECK(FuzzyScore(Str8Lit("core/editor/command.cpp"), Str8Lit("cmd")) != kFuzzyNoMatch);
  CHECK_EQ(FuzzyScore(Str8Lit("core/editor/command.cpp"), Str8Lit("zzz")), kFuzzyNoMatch);
  // An empty query matches everything.
  CHECK_EQ(FuzzyScore(Str8Lit("anything"), Str8Lit("")), 0);

  // Matches on segment boundaries beat the same letters buried mid-word.
  i32 boundary = FuzzyScore(Str8Lit("core/vim/vim_motions.cpp"), Str8Lit("cvm"));
  i32 scattered = FuzzyScore(Str8Lit("abcdefghijklmnopqrstuvwxyz"), Str8Lit("cvm"));
  CHECK(boundary > scattered);

  // Consecutive characters beat spread-out ones.
  CHECK(FuzzyScore(Str8Lit("command.cpp"), Str8Lit("comm")) >
        FuzzyScore(Str8Lit("c_o_m_m_a_n_d.cpp"), Str8Lit("comm")));
}

TEST(fuzzy_filter_orders_best_first) {
  Arena *arena = ArenaAlloc(MB(4));

  String8 candidates[] = {
      Str8Lit("app/render/draw.cpp"),
      Str8Lit("core/vim/vim_motions.cpp"),
      Str8Lit("core/vim/vim_operators.cpp"),
      Str8Lit("core/editor/command.cpp"),
  };

  FuzzyResults results = FuzzyFilter(arena, candidates, ArrayCount(candidates),
                                     Str8Lit("vimop"), 10);
  CHECK(results.count >= 1);
  CHECK_STR(results.items[0].text, Str8Lit("core/vim/vim_operators.cpp"));

  // Non-matches are dropped rather than ranked last.
  FuzzyResults none = FuzzyFilter(arena, candidates, ArrayCount(candidates),
                                  Str8Lit("qqqq"), 10);
  CHECK_EQ(none.count, 0);

  // An empty query keeps everything, in the original order.
  FuzzyResults all = FuzzyFilter(arena, candidates, ArrayCount(candidates),
                                 String8{nullptr, 0}, 10);
  CHECK_EQ(all.count, ArrayCount(candidates));
  CHECK_STR(all.items[0].text, candidates[0]);

  // The limit is honoured.
  CHECK_EQ(FuzzyFilter(arena, candidates, ArrayCount(candidates), String8{nullptr, 0}, 2).count, 2);

  ArenaRelease(arena);
}

TEST(picker_buffers_are_ordinary_buffers) {
  Tree tree = MakeTree("picker");

  Arena *arena = ArenaAlloc(MB(64));
  Editor ed = {};
  EditorInit(&ed, arena, RectS32{0, 0, 80, 25});
  ed.cwd = PushStr8Copy(arena, tree.root);

  // Grep results: a read-only buffer whose keymap claims <CR>.
  CHECK(CommandExecLine(&ed, Str8Lit("grep needle")));
  Buffer *grep = EditorFocusedBuffer(&ed);
  CHECK_STR(grep->name, Str8Lit("[grep]"));
  CHECK(BufferIsReadOnly(grep));
  CHECK(grep->hooks.keymap != nullptr);
  CHECK(grep->hooks.on_submit != nullptr);

  String8 text = BufferTextAll(arena, grep);
  CHECK(Str8FindFirst(text, Str8Lit("src/one.cpp")) < text.size);

  // <CR> opens whatever the selected line names, at the line it names. Which
  // result comes first depends on walk order, so read it rather than assume.
  String8 selected = BufferLineText(arena, grep, 2);
  u64 colon = Str8FindFirstChar(selected, ':');
  String8 selected_path = Str8Prefix(selected, colon);
  u64 selected_line = 0;
  for (u64 i = colon + 1; i < selected.size && CharIsDigit(selected.str[i]); i += 1) {
    selected_line = selected_line * 10 + (u64)(selected.str[i] - '0');
  }
  CHECK(selected_path.size > 0);
  CHECK(selected_line > 0);

  EditorProcessSpec(&ed, "<CR>");
  Buffer *opened = EditorFocusedBuffer(&ed);
  CHECK(Str8EndsWith(opened->path, selected_path));
  CHECK_EQ(ViewCursorLine(EditorFocusedView(&ed), opened), selected_line - 1);

  // The finder: a query on line 0, results below, refiltered as it changes.
  CHECK(CommandExecLine(&ed, Str8Lit("find")));
  Buffer *finder = EditorFocusedBuffer(&ed);
  CHECK_STR(finder->name, Str8Lit("[files]"));
  CHECK(finder->hooks.on_edit != nullptr);
  u64 unfiltered = BufferLineCount(finder);
  CHECK(unfiltered > 1);

  EditorProcessSpec(&ed, "two");
  CHECK_STR(BufferLineText(arena, finder, 0), Str8Lit("two"));
  // Typing narrows the list without disturbing the query line.
  CHECK(BufferLineCount(finder) < unfiltered);
  CHECK_STR(BufferLineText(arena, finder, 1), Str8Lit("src/deep/two.cpp"));

  // Enter while still typing opens the top result, as it does for live search.
  EditorProcessSpec(&ed, "<CR>");
  CHECK(Str8EndsWith(EditorFocusedBuffer(&ed)->path, Str8Lit("src/deep/two.cpp")));

  EditorDestroy(&ed);
  ArenaRelease(arena);
  Destroy(&tree);
}
