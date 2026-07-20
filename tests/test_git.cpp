#include "buffers/buf_git.h"
#include "buffers/git_ops.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"
#include "test.h"
#include "test_tempdir.h"

#include <initializer_list>

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
  TempDir dir;
};

bool HaveGit() {
  TempArena scratch = ScratchBegin();
  bool ok = GitAvailable(scratch.arena);
  ScratchEnd(scratch);
  return ok;
}

GitResult RunIn(Arena *arena, String8 cwd, std::initializer_list<String8> args) {
  String8 *list = PushArray(arena, String8, args.size());
  u64 i = 0;
  for (String8 a : args) list[i++] = a;
  return GitRun(arena, cwd, list, args.size());
}

bool InitRepo(Fixture *f) {
  if (!HaveGit()) return false;
  GitResult init = RunIn(f->arena, f->dir.path, {Str8Lit("init")});
  if (!init.started || init.exit_code != 0) return false;
  (void)RunIn(f->arena, f->dir.path, {Str8Lit("config"), Str8Lit("user.email"), Str8Lit("t@t.test")});
  (void)RunIn(f->arena, f->dir.path, {Str8Lit("config"), Str8Lit("user.name"), Str8Lit("Test")});
  // Avoid depending on the platform default branch name in assertions.
  (void)RunIn(f->arena, f->dir.path, {Str8Lit("checkout"), Str8Lit("-b"), Str8Lit("main")});
  return true;
}

Fixture MakeFixture(const char *tag) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  f.dir = MakeTempDir(tag);
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});
  f.ed.cwd = PushStr8Copy(f.arena, f.dir.path);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  Destroy(&f->dir);
  ArenaRelease(f->arena);
}

bool Contains(String8 haystack, String8 needle) {
  return Str8FindFirst(haystack, needle) < haystack.size;
}

Buffer *GitStatusBuffer(Fixture *f) {
  BufferHandle handle = BufferFromName(&f->ed.buffers, Str8Lit("[git]"));
  return BufferFromHandle(&f->ed.buffers, handle);
}

String8 BufferAll(Arena *arena, Buffer *buffer) { return BufferTextAll(arena, buffer); }

void WriteFile(Fixture *f, const char *rel, String8 contents) {
  CHECK(OsFileWrite(TempPath(&f->dir, rel), contents));
}

}  // namespace

TEST(git_flags_render_and_append) {
  Arena *arena = ArenaAlloc(MB(4));
  GitFlags flags = {};
  CHECK_STR(GitFlagsRender(arena, flags), Str8Lit("none"));

  flags.rebase = true;
  flags.autostash = true;
  CHECK_STR(GitFlagsRender(arena, flags), Str8Lit("--rebase --autostash"));

  String8List pull = {};
  GitFlagsAppendPull(arena, &pull, flags);
  CHECK_EQ(pull.node_count, 2);

  flags.set_upstream = true;
  String8List push = {};
  GitFlagsAppendPush(arena, &push, flags);
  CHECK_EQ(push.node_count, 1);
  CHECK_STR(push.first->string, Str8Lit("-u"));

  ArenaRelease(arena);
}

TEST(git_parse_status_splits_staged_and_unstaged) {
  Arena *arena = ArenaAlloc(MB(4));
  String8 porcelain = Str8Lit(
      "## main...origin/main [ahead 1, behind 2]\n"
      " M dirty.cpp\n"
      "A  staged.cpp\n"
      "MM both.cpp\n"
      "?? untracked.txt\n");
  GitStatus status = GitParseStatus(arena, porcelain);
  CHECK_STR(status.branch.head, Str8Lit("main"));
  CHECK_STR(status.branch.upstream, Str8Lit("origin/main"));
  CHECK_EQ(status.branch.ahead, 1);
  CHECK_EQ(status.branch.behind, 2);

  u64 untracked = 0, unstaged = 0, staged = 0;
  for (u64 i = 0; i < status.count; i += 1) {
    if (status.entries[i].kind == GitEntryKind::Untracked) untracked += 1;
    if (status.entries[i].kind == GitEntryKind::Unstaged) unstaged += 1;
    if (status.entries[i].kind == GitEntryKind::Staged) staged += 1;
  }
  CHECK_EQ(untracked, 1);
  CHECK_EQ(unstaged, 2);  // dirty + both
  CHECK_EQ(staged, 2);    // staged + both
  ArenaRelease(arena);
}

TEST(git_parse_diff_extracts_hunks) {
  Arena *arena = ArenaAlloc(MB(4));
  String8 diff = Str8Lit(
      "diff --git a/a.txt b/a.txt\n"
      "index 111..222 100644\n"
      "--- a/a.txt\n"
      "+++ b/a.txt\n"
      "@@ -1,2 +1,3 @@\n"
      " line1\n"
      "-line2\n"
      "+line2 changed\n"
      "+line3\n");
  GitDiff parsed = GitParseDiff(arena, diff);
  CHECK_EQ(parsed.file_count, 1);
  CHECK_STR(parsed.files[0].path, Str8Lit("a.txt"));
  CHECK_EQ(parsed.files[0].hunk_count, 1);
  CHECK_EQ(parsed.files[0].hunks[0].old_start, 1);
  CHECK_EQ(parsed.files[0].hunks[0].new_start, 1);

  String8 patch = GitHunkPatch(arena, &parsed.files[0], &parsed.files[0].hunks[0]);
  CHECK(Contains(patch, Str8Lit("diff --git")));
  CHECK(Contains(patch, Str8Lit("@@ -1,2 +1,3 @@")));
  CHECK(Contains(patch, Str8Lit("+line3")));
  ArenaRelease(arena);
}

TEST(git_status_buffer_lists_changes) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_status");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }

  WriteFile(&f, "tracked.txt", Str8Lit("hello\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("tracked.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("init")}).exit_code,
           0);

  WriteFile(&f, "tracked.txt", Str8Lit("hello\nworld\n"));
  WriteFile(&f, "new.txt", Str8Lit("fresh\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("new.txt")}).exit_code, 0);

  CommandExecLine(&f.ed, Str8Lit("git"));
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK_EQ((u32)buffer->kind, (u32)BufferKind::Git);

  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  CHECK(Contains(text, Str8Lit("Unstaged changes")));
  CHECK(Contains(text, Str8Lit("Staged changes")));
  CHECK(Contains(text, Str8Lit("tracked.txt")));
  CHECK(Contains(text, Str8Lit("new.txt")));
  CHECK(Contains(text, Str8Lit("Args: none")));
  ScratchEnd(scratch);

  Destroy(&f);
}

TEST(git_stage_file_and_commit) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_stage");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }

  WriteFile(&f, "a.txt", Str8Lit("one\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("base")}).exit_code,
           0);
  WriteFile(&f, "a.txt", Str8Lit("one\ntwo\n"));

  CommandExecLine(&f.ed, Str8Lit("git"));
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);

  // Put the cursor on the unstaged file line and stage it.
  View *view = EditorFocusedView(&f.ed);
  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  u64 pos = Str8FindFirst(text, Str8Lit("a.txt"));
  CHECK(pos < text.size);
  u64 line = BufferLineFromOffset(buffer, pos);
  ViewSetCursorLineColumn(view, buffer, line, 0);
  ScratchEnd(scratch);

  CommandExec(&f.ed, CommandId::git_stage, String8{}, KeyChord{});
  buffer = GitStatusBuffer(&f);
  scratch = ScratchBegin();
  text = BufferAll(scratch.arena, buffer);
  // After staging, a.txt should appear under Staged.
  CHECK(Contains(text, Str8Lit("Staged changes")));
  ScratchEnd(scratch);

  CommandExecLine(&f.ed, Str8Lit("git-commit stage me"));
  GitLog log = GitLoadLog(f.arena, f.dir.path, 5);
  CHECK(log.count >= 2);
  CHECK(Contains(log.entries[0].subject, Str8Lit("stage me")));

  Destroy(&f);
}

TEST(git_arg_rebase_toggle_affects_pull_argv) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_flags");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }
  WriteFile(&f, "a.txt", Str8Lit("x\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("c")}).exit_code, 0);

  CommandExecLine(&f.ed, Str8Lit("git"));
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);

  GitFlags before = GitBufferFlags(buffer);
  CHECK(!before.rebase);

  CommandExec(&f.ed, CommandId::git_arg_rebase, String8{}, KeyChord{});
  GitFlags after = GitBufferFlags(buffer);
  CHECK(after.rebase);

  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  CHECK(Contains(text, Str8Lit("Args: --rebase")));
  ScratchEnd(scratch);

  // Unit-level: flags append --rebase for pull.
  String8List argv = {};
  GitFlagsAppendPull(f.arena, &argv, after);
  CHECK_EQ(argv.node_count, 1);
  CHECK_STR(argv.first->string, Str8Lit("--rebase"));

  Destroy(&f);
}

TEST(git_log_lists_commits) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_log");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }
  WriteFile(&f, "a.txt", Str8Lit("x\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(
      RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("first commit")}).exit_code,
      0);

  CommandExecLine(&f.ed, Str8Lit("git-log"));
  BufferHandle handle = BufferFromName(&f.ed.buffers, Str8Lit("[git-log]"));
  Buffer *buffer = BufferFromHandle(&f.ed.buffers, handle);
  CHECK(buffer != nullptr);

  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  CHECK(Contains(text, Str8Lit("first commit")));
  ScratchEnd(scratch);

  Destroy(&f);
}

TEST(git_outside_repo_reports_status) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_none");
  // cwd is an empty temp dir with no .git
  CommandExecLine(&f.ed, Str8Lit("git"));
  CHECK(GitStatusBuffer(&f) == nullptr);
  CHECK(Contains(f.ed.status_message, Str8Lit("not a git repository")));
  Destroy(&f);
}

TEST(git_leader_gs_opens_status) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_leader");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }
  WriteFile(&f, "a.txt", Str8Lit("x\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("c")}).exit_code, 0);

  EditorProcessSpec(&f.ed, "<leader>gs");
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK(EditorFocusedBuffer(&f.ed) == buffer);
  Destroy(&f);
}
