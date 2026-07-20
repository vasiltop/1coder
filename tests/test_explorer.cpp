#include "buffers/buf_explorer.h"
#include "buffers/buf_image.h"
#include "buffers/explorer_ops.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "editor/filetype.h"
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

// ---------------------------------------------------------------------------
// The explorer as a buffer: navigation, editing, jumps
// ---------------------------------------------------------------------------

namespace {

struct EditorFixture {
  Arena *arena;
  Editor ed;
  TempDir dir;
};

// A small tree to navigate: two files at the top and a subdirectory holding
// one more.
EditorFixture MakeEditorFixture(const char *tag) {
  EditorFixture f = {};
  f.arena = ArenaAlloc(MB(64));
  f.dir = MakeTempDir(tag);
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});

  CHECK(OsMakeDir(TempPath(&f.dir, "sub")));
  CHECK(OsFileWrite(TempPath(&f.dir, "sub/deep.txt"), Str8Lit("deep\n")));
  CHECK(OsFileWrite(TempPath(&f.dir, "alpha.txt"), Str8Lit("alpha\n")));
  CHECK(OsFileWrite(TempPath(&f.dir, "beta.txt"), Str8Lit("beta\n")));

  return f;
}

void Destroy(EditorFixture *f) {
  EditorDestroy(&f->ed);
  Destroy(&f->dir);
  ArenaRelease(f->arena);
}

Buffer *FocusedBuffer(EditorFixture *f) { return EditorFocusedBuffer(&f->ed); }

// The name on the cursor line, with any id and marker stripped.
String8 CursorEntryName(EditorFixture *f) {
  Buffer *buffer = FocusedBuffer(f);
  View *view = EditorFocusedView(&f->ed);
  String8 line = BufferLineText(f->arena, buffer, ViewCursorLine(view, buffer));
  return ExplorerParseLine(line, BufferLineCount(buffer) + 1).name;
}

void Keys(EditorFixture *f, const char *spec) { EditorProcessSpec(&f->ed, Str8C(spec)); }

}  // namespace

TEST(explorer_dash_from_a_file_opens_its_directory) {
  EditorFixture f = MakeEditorFixture("explorer_dash");

  BufferHandle file = EditorOpenFile(&f.ed, TempPath(&f.dir, "beta.txt"));
  CHECK(file.index != 0);
  EditorShowBuffer(&f.ed, file);
  CHECK_EQ((u32)FocusedBuffer(&f)->kind, (u32)BufferKind::File);

  Keys(&f, "-");

  // The listing for the containing directory, with the cursor sitting on the
  // file we came from -- the whole point of oil's `-`.
  Buffer *buffer = FocusedBuffer(&f);
  CHECK_EQ((u32)buffer->kind, (u32)BufferKind::Explorer);
  CHECK_STR(ExplorerBufferDir(buffer), OsPathAbsolute(f.arena, f.dir.path));
  CHECK_STR(CursorEntryName(&f), Str8Lit("beta.txt"));

  Destroy(&f);
}

TEST(explorer_enter_opens_files_and_directories) {
  EditorFixture f = MakeEditorFixture("explorer_enter");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Listing order is directories first, then names: sub/, alpha.txt, beta.txt.
  CHECK_STR(CursorEntryName(&f), Str8Lit("sub/"));

  Keys(&f, "<CR>");
  CHECK_EQ((u32)FocusedBuffer(&f)->kind, (u32)BufferKind::Explorer);
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)),
            OsPathAbsolute(f.arena, TempPath(&f.dir, "sub")));
  CHECK_STR(CursorEntryName(&f), Str8Lit("deep.txt"));

  Keys(&f, "<CR>");
  Buffer *opened = FocusedBuffer(&f);
  CHECK_EQ((u32)opened->kind, (u32)BufferKind::File);
  CHECK_STR(BufferTextAll(f.arena, opened), Str8Lit("deep"));

  Destroy(&f);
}

TEST(explorer_dash_climbs_and_lands_on_the_child) {
  EditorFixture f = MakeEditorFixture("explorer_climb");

  BufferHandle sub = ExplorerBufferOpen(&f.ed, TempPath(&f.dir, "sub"));
  EditorShowBuffer(&f.ed, sub);

  Keys(&f, "-");

  // Going up lands on the directory just left, so `-` and <CR> are inverses.
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)), OsPathAbsolute(f.arena, f.dir.path));
  CHECK_STR(CursorEntryName(&f), Str8Lit("sub/"));

  Destroy(&f);
}

TEST(explorer_dash_at_the_root_does_nothing) {
  EditorFixture f = MakeEditorFixture("explorer_root");

  BufferHandle root = ExplorerBufferOpen(&f.ed, Str8Lit("/"));
  CHECK(root.index != 0);
  EditorShowBuffer(&f.ed, root);

  // The parent of "/" is "/", so this must not push a jump or churn the buffer.
  Keys(&f, "-");
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)), Str8Lit("/"));

  Destroy(&f);
}

TEST(explorer_shares_one_buffer_per_directory) {
  EditorFixture f = MakeEditorFixture("explorer_dedupe");

  BufferHandle a = ExplorerBufferOpen(&f.ed, f.dir.path);
  BufferHandle b = ExplorerBufferOpen(&f.ed, f.dir.path);
  CHECK(BufferHandleEqual(a, b));

  // A trailing separator names the same directory and must not fork a buffer,
  // or the two copies would diff against different snapshots.
  BufferHandle c = ExplorerBufferOpen(&f.ed, PushStr8Cat(f.arena, f.dir.path, Str8Lit("/")));
  CHECK(BufferHandleEqual(a, c));

  // :e on a directory is the same door.
  BufferHandle d = EditorOpenFile(&f.ed, f.dir.path);
  CHECK(BufferHandleEqual(a, d));

  Destroy(&f);
}

TEST(explorer_keeps_every_vim_binding) {
  EditorFixture f = MakeEditorFixture("explorer_vim");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // A buffer-local map layers above the mode map rather than replacing it, so
  // motions still work in a buffer that has claimed <CR> and `-`.
  Keys(&f, "j");
  CHECK_STR(CursorEntryName(&f), Str8Lit("alpha.txt"));
  Keys(&f, "G");
  CHECK_STR(CursorEntryName(&f), Str8Lit("beta.txt"));
  Keys(&f, "gg");
  CHECK_STR(CursorEntryName(&f), Str8Lit("sub/"));

  // And so does editing, which is what makes a rename `cw`.
  Keys(&f, "jA_edited<Esc>");
  CHECK(BufferIsDirty(FocusedBuffer(&f)));
  CHECK_STR(CursorEntryName(&f), Str8Lit("alpha.txt_edited"));

  Destroy(&f);
}

TEST(explorer_navigation_records_jumps) {
  EditorFixture f = MakeEditorFixture("explorer_jumps");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // root -> sub/ -> deep.txt
  Keys(&f, "<CR><CR>");
  CHECK_EQ((u32)FocusedBuffer(&f)->kind, (u32)BufferKind::File);
  CHECK_STR(FocusedBuffer(&f)->name, Str8Lit("deep.txt"));

  // <C-o> walks back through everywhere visited, directories included.
  Keys(&f, "<C-o>");
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)),
            OsPathAbsolute(f.arena, TempPath(&f.dir, "sub")));

  Keys(&f, "<C-o>");
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)), OsPathAbsolute(f.arena, f.dir.path));

  // And <C-i> forward again.
  Keys(&f, "<C-i>");
  CHECK_STR(ExplorerBufferDir(FocusedBuffer(&f)),
            OsPathAbsolute(f.arena, TempPath(&f.dir, "sub")));

  Destroy(&f);
}

TEST(explorer_enter_on_an_unwritten_line_does_not_open) {
  EditorFixture f = MakeEditorFixture("explorer_unwritten");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // A typed line names nothing on disk yet, so Enter must not conjure a buffer
  // for a path the user has not agreed to create.
  Keys(&f, "Gohello.txt<Esc>");
  CHECK_STR(CursorEntryName(&f), Str8Lit("hello.txt"));

  Keys(&f, "<CR>");
  CHECK_EQ((u32)FocusedBuffer(&f)->kind, (u32)BufferKind::Explorer);
  CHECK(!OsFileExists(TempPath(&f.dir, "hello.txt")));

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// :w -- confirmation and apply
// ---------------------------------------------------------------------------

namespace {

void RunCommand(EditorFixture *f, const char *line) {
  CommandExecLine(&f->ed, Str8C(line));
}

bool StatusContains(EditorFixture *f, const char *needle) {
  String8 status = f->ed.status_message;
  return Str8FindFirst(status, Str8C(needle)) < status.size;
}

}  // namespace

TEST(explorer_write_asks_before_touching_anything) {
  EditorFixture f = MakeEditorFixture("explorer_confirm");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Rename alpha.txt -> renamed.txt by editing its line.
  Keys(&f, "jA_x<Esc>");
  RunCommand(&f, "w");

  // Nothing has happened yet: the plan is only described.
  CHECK(StatusContains(&f, "Apply? [y/N]"));
  CHECK(OsFileExists(TempPath(&f.dir, "alpha.txt")));
  CHECK(f.ed.input.awaiting_confirm);

  // Any key but y cancels, so a fumbled confirmation is never the destructive
  // answer.
  Keys(&f, "n");
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK(StatusContains(&f, "cancelled"));
  CHECK(OsFileExists(TempPath(&f.dir, "alpha.txt")));

  // The edit is still in the buffer, so the answer can be reconsidered.
  CHECK(BufferIsDirty(FocusedBuffer(&f)));
  RunCommand(&f, "w");
  Keys(&f, "y");

  CHECK(!OsFileExists(TempPath(&f.dir, "alpha.txt")));
  CHECK(OsFileExists(TempPath(&f.dir, "alpha.txt_x")));
  // Contents follow the rename -- the whole reason ids exist.
  CHECK_STR(OsFileRead(f.arena, TempPath(&f.dir, "alpha.txt_x")).data, Str8Lit("alpha\n"));

  // The listing was reread, so it is clean and matches disk again.
  CHECK(!BufferIsDirty(FocusedBuffer(&f)));
  CHECK_EQ(BufferLineCount(FocusedBuffer(&f)), (u64)3);

  Destroy(&f);
}

TEST(explorer_write_creates_and_deletes_on_confirm) {
  EditorFixture f = MakeEditorFixture("explorer_apply_keys");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Delete beta.txt with dd, then add a file and a directory by typing lines.
  Keys(&f, "Gdd");
  Keys(&f, "Gonotes.md<Esc>");
  Keys(&f, "odocs/<Esc>");
  RunCommand(&f, "w");
  Keys(&f, "y");

  CHECK(!OsFileExists(TempPath(&f.dir, "beta.txt")));
  CHECK(OsFileExists(TempPath(&f.dir, "notes.md")));
  CHECK(OsDirExists(TempPath(&f.dir, "docs")));
  // Untouched entries are left alone.
  CHECK(OsFileExists(TempPath(&f.dir, "alpha.txt")));

  Destroy(&f);
}

TEST(explorer_write_with_no_edits_does_nothing) {
  EditorFixture f = MakeEditorFixture("explorer_noop");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Reordering lines is not a change: the diff never consults order.
  Keys(&f, "ddp");
  RunCommand(&f, "w");

  CHECK(StatusContains(&f, "no changes"));
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK(!BufferIsDirty(FocusedBuffer(&f)));

  Destroy(&f);
}

TEST(explorer_write_refuses_a_duplicated_id) {
  EditorFixture f = MakeEditorFixture("explorer_dupe_write");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // yyp duplicates a line id and all. Copy and rename are both plausible
  // readings, so nothing is armed and nothing runs.
  Keys(&f, "jyyp");
  RunCommand(&f, "w");

  CHECK(StatusContains(&f, "duplicate id"));
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK(OsFileExists(TempPath(&f.dir, "alpha.txt")));

  Destroy(&f);
}

TEST(explorer_write_never_writes_the_listing_to_disk) {
  EditorFixture f = MakeEditorFixture("explorer_no_clobber");

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  RunCommand(&f, "w");

  // The on_write hook claims the write, so BufferSaveFile never runs -- which
  // would otherwise try to write the listing text over the directory inode.
  CHECK(OsDirExists(f.dir.path));
  CHECK(!OsFileExists(f.dir.path));

  Destroy(&f);
}

TEST(explorer_rename_carries_an_open_buffer_with_it) {
  EditorFixture f = MakeEditorFixture("explorer_retarget");

  BufferHandle file = EditorOpenFile(&f.ed, TempPath(&f.dir, "alpha.txt"));
  Buffer *opened = BufferFromHandle(&f.ed.buffers, file);
  CHECK(opened != nullptr);

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  Keys(&f, "jA_moved<Esc>");
  RunCommand(&f, "w");
  Keys(&f, "y");

  // The open buffer followed its file, so a later :w goes to the new path
  // rather than recreating the old one.
  CHECK_STR(opened->path, OsPathAbsolute(f.arena, TempPath(&f.dir, "alpha.txt_moved")));
  CHECK_STR(opened->name, Str8Lit("alpha.txt_moved"));

  Destroy(&f);
}

TEST(explorer_deleting_an_open_file_keeps_its_text) {
  EditorFixture f = MakeEditorFixture("explorer_orphan");

  BufferHandle file = EditorOpenFile(&f.ed, TempPath(&f.dir, "alpha.txt"));
  Buffer *opened = BufferFromHandle(&f.ed.buffers, file);
  CHECK(opened != nullptr);

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  Keys(&f, "jdd");
  RunCommand(&f, "w");
  Keys(&f, "y");

  CHECK(!OsFileExists(TempPath(&f.dir, "alpha.txt")));

  // Closing the buffer would be the one outcome that loses work with no way
  // back. Instead it is left dirty, so :w puts the file back.
  CHECK_STR(BufferTextAll(f.arena, opened), Str8Lit("alpha"));
  CHECK(BufferIsDirty(opened));

  Destroy(&f);
}

TEST(explorer_directory_rename_moves_the_buffers_inside_it) {
  EditorFixture f = MakeEditorFixture("explorer_dir_rename");

  BufferHandle deep = EditorOpenFile(&f.ed, TempPath(&f.dir, "sub/deep.txt"));
  Buffer *opened = BufferFromHandle(&f.ed.buffers, deep);
  CHECK(opened != nullptr);

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Rename the directory itself. sub/ is the first line, being a directory, and
  // the insert goes before the trailing marker so the name stays a directory.
  Keys(&f, "$i_renamed<Esc>");
  RunCommand(&f, "w");
  Keys(&f, "y");

  CHECK(OsDirExists(TempPath(&f.dir, "sub_renamed")));
  CHECK(!OsDirExists(TempPath(&f.dir, "sub")));

  // Matching on the directory prefix is what carries the buffers inside along.
  CHECK_STR(opened->path, OsPathAbsolute(f.arena, TempPath(&f.dir, "sub_renamed/deep.txt")));

  Destroy(&f);
}

TEST(explorer_a_failed_move_leaves_its_buffer_alone) {
  EditorFixture f = MakeEditorFixture("explorer_failed_move");

  BufferHandle deep = EditorOpenFile(&f.ed, TempPath(&f.dir, "sub/deep.txt"));
  Buffer *opened = BufferFromHandle(&f.ed.buffers, deep);
  CHECK(opened != nullptr);
  String8 original = PushStr8Copy(f.arena, opened->path);

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // Appending past the trailing marker asks to move sub/ inside itself, which
  // the kernel refuses. A plan describes what was asked for, not what happened,
  // so the buffer must not follow a move that never occurred.
  Keys(&f, "A_nested<Esc>");
  RunCommand(&f, "w");
  Keys(&f, "y");

  CHECK(StatusContains(&f, "failed"));
  CHECK(OsDirExists(TempPath(&f.dir, "sub")));
  CHECK_STR(opened->path, original);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Filetype dispatch and images
// ---------------------------------------------------------------------------

namespace {

BufferHandle FakeXyzOpen(Editor *ed, String8 path) {
  (void)path;
  return BufferOpen(&ed->buffers, BufferKind::Scratch, Str8Lit("[xyz]"));
}

// The smallest legal PNG header: signature, then an IHDR chunk whose payload
// starts with the dimensions.
void WriteFakePng(TempDir *dir, const char *name, u32 width, u32 height) {
  u8 bytes[24] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
                  0,    0,   0,   13,  'I',  'H',  'D',  'R'};
  for (u32 i = 0; i < 4; i += 1) {
    bytes[16 + i] = (u8)(width >> (24 - 8 * i));
    bytes[20 + i] = (u8)(height >> (24 - 8 * i));
  }
  CHECK(OsFileWrite(TempPath(dir, name), String8{bytes, sizeof(bytes)}));
}

}  // namespace

TEST(filetype_dispatch_picks_handlers_by_extension) {
  EditorFixture f = MakeEditorFixture("filetype");

  FiletypeRegister(&f.ed, Str8Lit("xyz"), FakeXyzOpen);
  CHECK(OsFileWrite(TempPath(&f.dir, "a.xyz"), Str8Lit("x")));
  CHECK(OsFileWrite(TempPath(&f.dir, "b.XYZ"), Str8Lit("x")));

  Buffer *opened = BufferFromHandle(&f.ed.buffers, FiletypeOpen(&f.ed, TempPath(&f.dir, "a.xyz")));
  CHECK(opened != nullptr);
  CHECK_STR(opened->name, Str8Lit("[xyz]"));

  // Extensions are matched case-insensitively, since the filesystem's case is
  // not the user's intent.
  opened = BufferFromHandle(&f.ed.buffers, FiletypeOpen(&f.ed, TempPath(&f.dir, "b.XYZ")));
  CHECK_STR(opened->name, Str8Lit("[xyz]"));

  // Anything unregistered is text, and a directory is the explorer.
  opened = BufferFromHandle(&f.ed.buffers, FiletypeOpen(&f.ed, TempPath(&f.dir, "alpha.txt")));
  CHECK_EQ((u32)opened->kind, (u32)BufferKind::File);

  opened = BufferFromHandle(&f.ed.buffers, FiletypeOpen(&f.ed, TempPath(&f.dir, "sub")));
  CHECK_EQ((u32)opened->kind, (u32)BufferKind::Explorer);

  Destroy(&f);
}

TEST(image_buffer_reads_dimensions_from_the_header) {
  EditorFixture f = MakeEditorFixture("image");

  WriteFakePng(&f.dir, "picture.png", 1920, 1080);

  Buffer *buffer =
      BufferFromHandle(&f.ed.buffers, ImageBufferOpen(&f.ed, TempPath(&f.dir, "picture.png")));
  CHECK(buffer != nullptr);
  CHECK_EQ((u32)buffer->kind, (u32)BufferKind::Image);

  const ImageInfo *info = ImageBufferInfo(buffer);
  CHECK(info != nullptr);
  CHECK_EQ(info->width, (u32)1920);
  CHECK_EQ(info->height, (u32)1080);
  CHECK_STR(info->format, Str8Lit("PNG"));

  // The summary is the buffer's text, so the feature is useful with no
  // renderer support at all.
  String8 text = BufferTextAll(f.arena, buffer);
  CHECK(Str8FindFirst(text, Str8Lit("1920x1080")) < text.size);
  CHECK(Str8FindFirst(text, Str8Lit("picture.png")) < text.size);

  // Read-only with no on_write, so :w cannot write the summary over the image.
  CHECK(BufferIsReadOnly(buffer));

  Destroy(&f);
}

TEST(image_buffer_survives_a_truncated_header) {
  EditorFixture f = MakeEditorFixture("image_truncated");

  // A valid signature and nothing behind it.
  u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  CHECK(OsFileWrite(TempPath(&f.dir, "broken.png"), String8{signature, sizeof(signature)}));

  Buffer *buffer =
      BufferFromHandle(&f.ed.buffers, ImageBufferOpen(&f.ed, TempPath(&f.dir, "broken.png")));
  CHECK(buffer != nullptr);

  const ImageInfo *info = ImageBufferInfo(buffer);
  CHECK_STR(info->format, Str8Lit("PNG"));
  CHECK_EQ(info->width, (u32)0);

  // Saying so beats showing nothing, which would read as a bug in the viewer.
  String8 text = BufferTextAll(f.arena, buffer);
  CHECK(Str8FindFirst(text, Str8Lit("dimensions unavailable")) < text.size);

  Destroy(&f);
}

TEST(explorer_enter_on_an_image_opens_an_image_buffer) {
  EditorFixture f = MakeEditorFixture("explorer_image");

  WriteFakePng(&f.dir, "aaa_shot.png", 640, 480);

  BufferHandle root = ExplorerBufferOpen(&f.ed, f.dir.path);
  EditorShowBuffer(&f.ed, root);

  // aaa_shot.png sorts first among the files, after the sub/ directory.
  Keys(&f, "j<CR>");

  Buffer *opened = FocusedBuffer(&f);
  CHECK_EQ((u32)opened->kind, (u32)BufferKind::Image);
  CHECK_EQ(ImageBufferInfo(opened)->width, (u32)640);

  // And <C-o> comes back, so images sit in the jump list like anything else.
  Keys(&f, "<C-o>");
  CHECK_EQ((u32)FocusedBuffer(&f)->kind, (u32)BufferKind::Explorer);

  Destroy(&f);
}
