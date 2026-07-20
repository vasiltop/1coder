#include "buffers/explorer_ops.h"
#include "os/os_file.h"
#include "test.h"
#include "test_tempdir.h"

namespace {

// Builds a snapshot without touching the filesystem, so the diff can be driven
// entirely from literals.
ExplorerSnapshot FakeSnapshot(Arena *arena, String8 dir, const char **names, u64 count) {
  ExplorerSnapshot snap = {};
  snap.dir = PushStr8Copy(arena, dir);
  snap.entries = PushArray(arena, ExplorerEntry, Max(count, (u64)1));
  snap.count = count;

  for (u64 i = 0; i < count; i += 1) {
    String8 name = Str8C(names[i]);
    snap.entries[i].id = i + 1;
    // A trailing slash in the fixture marks a directory, matching the listing.
    snap.entries[i].is_dir = name.size > 0 && name.str[name.size - 1] == '/';
    if (snap.entries[i].is_dir) name = Str8Chop(name, 1);
    snap.entries[i].name = PushStr8Copy(arena, name);
  }
  return snap;
}

ExplorerPlan DiffLines(Arena *arena, const ExplorerSnapshot *snap, const char **text, u64 count) {
  String8 *lines = PushArray(arena, String8, Max(count, (u64)1));
  for (u64 i = 0; i < count; i += 1) lines[i] = Str8C(text[i]);
  return ExplorerDiff(arena, snap, lines, count);
}

u64 CountOps(const ExplorerPlan *plan, ExplorerOpKind kind) {
  u64 n = 0;
  for (u64 i = 0; i < plan->count; i += 1) {
    if (plan->ops[i].kind == kind) n += 1;
  }
  return n;
}

const ExplorerOp *FindOp(const ExplorerPlan *plan, ExplorerOpKind kind) {
  for (u64 i = 0; i < plan->count; i += 1) {
    if (plan->ops[i].kind == kind) return &plan->ops[i];
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(explorer_parse_line_reads_ids) {
  ExplorerLine parsed = ExplorerParseLine(Str8Lit("003 main.cpp"), 5);
  CHECK_EQ(parsed.id, (u64)3);
  CHECK_STR(parsed.name, Str8Lit("main.cpp"));
  CHECK(!parsed.blank);

  // Directories keep their marker; the diff uses it to pick mkdir over touch.
  parsed = ExplorerParseLine(Str8Lit("001 src/"), 5);
  CHECK_EQ(parsed.id, (u64)1);
  CHECK_STR(parsed.name, Str8Lit("src/"));

  // The link marker is decoration and comes back off.
  parsed = ExplorerParseLine(Str8Lit("002 link@"), 5);
  CHECK_EQ(parsed.id, (u64)2);
  CHECK_STR(parsed.name, Str8Lit("link"));

  // No id at all: a line the user typed, so a create.
  parsed = ExplorerParseLine(Str8Lit("brand_new.txt"), 5);
  CHECK_EQ(parsed.id, (u64)0);
  CHECK_STR(parsed.name, Str8Lit("brand_new.txt"));

  parsed = ExplorerParseLine(Str8Lit("   "), 5);
  CHECK(parsed.blank);
}

TEST(explorer_parse_line_digits_outside_range_are_a_name) {
  // Four entries, so ids run 1..4 and max_id is 5. A film named "12 Monkeys"
  // must not be read as a reference to id 12.
  ExplorerLine parsed = ExplorerParseLine(Str8Lit("12 Monkeys.mkv"), 5);
  CHECK_EQ(parsed.id, (u64)0);
  CHECK_STR(parsed.name, Str8Lit("12 Monkeys.mkv"));

  // Zero is never a valid id, so it is a name too.
  parsed = ExplorerParseLine(Str8Lit("0 notes"), 5);
  CHECK_EQ(parsed.id, (u64)0);
  CHECK_STR(parsed.name, Str8Lit("0 notes"));

  // Absurd digit runs must not overflow into a valid-looking id.
  parsed = ExplorerParseLine(Str8Lit("99999999999999999999999 x"), 5);
  CHECK_EQ(parsed.id, (u64)0);
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

TEST(explorer_diff_unchanged_and_reordered_produce_nothing) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"src/", "main.cpp", "README.md"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 3);

  const char *same[] = {"001 src/", "002 main.cpp", "003 README.md"};
  ExplorerPlan plan = DiffLines(arena, &snap, same, 3);
  CHECK_EQ(plan.error.size, (u64)0);
  CHECK_EQ(plan.count, (u64)0);

  // Order is never consulted, so sorting the buffer changes nothing on disk.
  const char *shuffled[] = {"003 README.md", "001 src/", "002 main.cpp"};
  plan = DiffLines(arena, &snap, shuffled, 3);
  CHECK_EQ(plan.count, (u64)0);

  // Blank lines are ignored, so a stray one never deletes anything.
  const char *with_blank[] = {"001 src/", "", "002 main.cpp", "  ", "003 README.md"};
  plan = DiffLines(arena, &snap, with_blank, 5);
  CHECK_EQ(plan.count, (u64)0);

  ArenaRelease(arena);
}

TEST(explorer_diff_detects_rename_not_delete_and_create) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"src/", "main.cpp"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 2);

  const char *renamed[] = {"001 src/", "002 parser.cpp"};
  ExplorerPlan plan = DiffLines(arena, &snap, renamed, 2);

  CHECK_EQ(plan.count, (u64)1);
  const ExplorerOp *move = FindOp(&plan, ExplorerOpKind::Move);
  CHECK(move != nullptr);
  CHECK_STR(move->from, Str8Lit("/proj/main.cpp"));
  CHECK_STR(move->to, Str8Lit("/proj/parser.cpp"));

  ArenaRelease(arena);
}

TEST(explorer_diff_moves_into_subdirectory) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"sub/", "x.c"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 2);

  const char *moved[] = {"001 sub/", "002 sub/x.c"};
  ExplorerPlan plan = DiffLines(arena, &snap, moved, 2);

  CHECK_EQ(plan.count, (u64)1);
  const ExplorerOp *move = FindOp(&plan, ExplorerOpKind::Move);
  CHECK(move != nullptr);
  CHECK_STR(move->from, Str8Lit("/proj/x.c"));
  CHECK_STR(move->to, Str8Lit("/proj/sub/x.c"));

  ArenaRelease(arena);
}

TEST(explorer_diff_creates_and_deletes) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"old.txt", "keep.txt"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 2);

  // Drop old.txt, add a file and a directory.
  const char *edited[] = {"002 keep.txt", "new.txt", "newdir/"};
  ExplorerPlan plan = DiffLines(arena, &snap, edited, 3);

  CHECK_EQ(plan.count, (u64)3);
  CHECK_EQ(CountOps(&plan, ExplorerOpKind::Delete), (u64)1);
  CHECK_EQ(CountOps(&plan, ExplorerOpKind::Create), (u64)1);
  CHECK_EQ(CountOps(&plan, ExplorerOpKind::CreateDir), (u64)1);

  CHECK_STR(FindOp(&plan, ExplorerOpKind::Delete)->from, Str8Lit("/proj/old.txt"));
  CHECK_STR(FindOp(&plan, ExplorerOpKind::Create)->to, Str8Lit("/proj/new.txt"));
  // The marker is what selects mkdir, and does not survive into the path.
  CHECK_STR(FindOp(&plan, ExplorerOpKind::CreateDir)->to, Str8Lit("/proj/newdir"));

  ArenaRelease(arena);
}

TEST(explorer_diff_deleting_a_directory_is_recursive) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"build/", "keep.txt"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 2);

  const char *edited[] = {"002 keep.txt"};
  ExplorerPlan plan = DiffLines(arena, &snap, edited, 1);

  CHECK_EQ(plan.count, (u64)1);
  CHECK_EQ(CountOps(&plan, ExplorerOpKind::DeleteDir), (u64)1);

  ArenaRelease(arena);
}

TEST(explorer_diff_refuses_duplicate_ids) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"a.txt", "b.txt"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 2);

  // What yyp produces. Copy and rename are both plausible readings, so the
  // plan refuses rather than guessing.
  const char *duped[] = {"001 a.txt", "001 a_copy.txt", "002 b.txt"};
  ExplorerPlan plan = DiffLines(arena, &snap, duped, 3);

  CHECK(plan.error.size > 0);
  CHECK_EQ(plan.count, (u64)0);

  ArenaRelease(arena);
}

TEST(explorer_diff_refuses_escaping_names) {
  Arena *arena = ArenaAlloc(MB(4));
  const char *names[] = {"a.txt"};
  ExplorerSnapshot snap = FakeSnapshot(arena, Str8Lit("/proj"), names, 1);

  const char *dotdot[] = {"001 ../escaped.txt"};
  CHECK(DiffLines(arena, &snap, dotdot, 1).error.size > 0);

  const char *nested_dotdot[] = {"001 sub/../../escaped.txt"};
  CHECK(DiffLines(arena, &snap, nested_dotdot, 1).error.size > 0);

  const char *absolute[] = {"001 /etc/passwd"};
  CHECK(DiffLines(arena, &snap, absolute, 1).error.size > 0);

  const char *dot[] = {"001 a.txt", "./x"};
  CHECK(DiffLines(arena, &snap, dot, 2).error.size > 0);

  ArenaRelease(arena);
}

// ---------------------------------------------------------------------------
// Scan / render round trip
// ---------------------------------------------------------------------------

TEST(explorer_render_round_trips_to_an_empty_plan) {
  TempDir dir = MakeTempDir("explorer_roundtrip");

  CHECK(OsFileWrite(TempPath(&dir, "zebra.txt"), Str8Lit("z")));
  CHECK(OsFileWrite(TempPath(&dir, "apple.txt"), Str8Lit("a")));
  CHECK(OsMakeDir(TempPath(&dir, "src")));

  ExplorerSnapshot snap = ExplorerScan(dir.arena, dir.path);
  CHECK_EQ(snap.count, (u64)3);
  // Directories first, matching OsDirList's order.
  CHECK_STR(snap.entries[0].name, Str8Lit("src"));
  CHECK(snap.entries[0].is_dir);

  String8 text = ExplorerRender(dir.arena, &snap);
  CHECK_STR(text, Str8Lit("001 src/\n002 apple.txt\n003 zebra.txt"));

  // Rendering then diffing the unmodified text must be a no-op, or every :w
  // without an edit would do work.
  String8List split = Str8SplitChar(dir.arena, text, '\n');
  String8 *lines = PushArray(dir.arena, String8, split.node_count);
  u64 n = 0;
  for (String8Node *node = split.first; node; node = node->next) lines[n++] = node->string;

  ExplorerPlan plan = ExplorerDiff(dir.arena, &snap, lines, n);
  CHECK_EQ(plan.error.size, (u64)0);
  CHECK_EQ(plan.count, (u64)0);

  Destroy(&dir);
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

TEST(explorer_apply_performs_every_operation) {
  TempDir dir = MakeTempDir("explorer_apply");

  CHECK(OsFileWrite(TempPath(&dir, "rename_me.txt"), Str8Lit("contents")));
  CHECK(OsFileWrite(TempPath(&dir, "delete_me.txt"), Str8Lit("x")));

  ExplorerSnapshot snap = ExplorerScan(dir.arena, dir.path);
  CHECK_EQ(snap.count, (u64)2);

  // delete_me.txt sorts first, so it is id 1.
  TempArena scratch = ScratchBegin1(dir.arena);
  String8 lines[] = {
      PushStr8F(scratch.arena, "002 renamed.txt"),
      PushStr8F(scratch.arena, "created.txt"),
      PushStr8F(scratch.arena, "created_dir/"),
  };

  ExplorerPlan plan = ExplorerDiff(dir.arena, &snap, lines, 3);
  CHECK_EQ(plan.error.size, (u64)0);
  CHECK_EQ(plan.count, (u64)4);

  String8 summary = ExplorerPlanSummary(dir.arena, &plan);
  CHECK(summary.size > 0);

  ExplorerApplyResult result = ExplorerApply(dir.arena, &plan);
  CHECK_EQ(result.failed, (u64)0);
  CHECK_EQ(result.applied, (u64)4);
  ScratchEnd(scratch);

  CHECK(!OsFileExists(TempPath(&dir, "delete_me.txt")));
  CHECK(!OsFileExists(TempPath(&dir, "rename_me.txt")));
  CHECK(OsFileExists(TempPath(&dir, "renamed.txt")));
  // A rename must carry the contents, which is the whole reason ids exist.
  CHECK_STR(OsFileRead(dir.arena, TempPath(&dir, "renamed.txt")).data, Str8Lit("contents"));
  CHECK(OsFileExists(TempPath(&dir, "created.txt")));
  CHECK(OsDirExists(TempPath(&dir, "created_dir")));

  Destroy(&dir);
}

TEST(explorer_apply_handles_swaps_and_rotations) {
  TempDir dir = MakeTempDir("explorer_swap");

  CHECK(OsFileWrite(TempPath(&dir, "a"), Str8Lit("A")));
  CHECK(OsFileWrite(TempPath(&dir, "b"), Str8Lit("B")));
  CHECK(OsFileWrite(TempPath(&dir, "c"), Str8Lit("C")));

  ExplorerSnapshot snap = ExplorerScan(dir.arena, dir.path);
  CHECK_EQ(snap.count, (u64)3);

  // a->b, b->c, c->a: a rotation, which naive in-place renaming would collapse
  // into a single surviving file. Staging every source under a temporary name
  // first is what makes it work.
  String8 lines[] = {Str8Lit("001 b"), Str8Lit("002 c"), Str8Lit("003 a")};
  ExplorerPlan plan = ExplorerDiff(dir.arena, &snap, lines, 3);
  CHECK_EQ(plan.count, (u64)3);

  ExplorerApplyResult result = ExplorerApply(dir.arena, &plan);
  CHECK_EQ(result.failed, (u64)0);
  CHECK_EQ(result.applied, (u64)3);

  CHECK_STR(OsFileRead(dir.arena, TempPath(&dir, "b")).data, Str8Lit("A"));
  CHECK_STR(OsFileRead(dir.arena, TempPath(&dir, "c")).data, Str8Lit("B"));
  CHECK_STR(OsFileRead(dir.arena, TempPath(&dir, "a")).data, Str8Lit("C"));

  Destroy(&dir);
}

TEST(explorer_apply_deletes_directories_recursively) {
  TempDir dir = MakeTempDir("explorer_rmdir");

  CHECK(OsMakeDirs(TempPath(&dir, "build/objects")));
  CHECK(OsFileWrite(TempPath(&dir, "build/objects/main.o"), Str8Lit("x")));

  ExplorerSnapshot snap = ExplorerScan(dir.arena, dir.path);
  CHECK_EQ(snap.count, (u64)1);

  ExplorerPlan plan = ExplorerDiff(dir.arena, &snap, nullptr, 0);
  CHECK_EQ(plan.count, (u64)1);
  CHECK_EQ(CountOps(&plan, ExplorerOpKind::DeleteDir), (u64)1);

  ExplorerApplyResult result = ExplorerApply(dir.arena, &plan);
  CHECK_EQ(result.failed, (u64)0);
  CHECK(!OsDirExists(TempPath(&dir, "build")));

  Destroy(&dir);
}
