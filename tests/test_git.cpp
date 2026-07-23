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

TokenKind TokenKindAt(Buffer *buffer, String8 text, String8 needle) {
  u64 offset = Str8FindFirst(text, needle);
  CHECK(offset < text.size);
  return TokenKindAtOffset(&buffer->tokens, offset);
}

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
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("Unstaged changes")),
           (u32)TokenKind::Keyword);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("tracked.txt")), (u32)TokenKind::String);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("Args:")), (u32)TokenKind::Keyword);
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

TEST(git_status_folds_single_hunk) {
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_fold");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }

  // A file whose top and bottom change, far enough apart to form two hunks.
  WriteFile(&f, "a.txt",
            Str8Lit("l01\nl02\nl03\nl04\nl05\nl06\nl07\nl08\nl09\nl10\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("base")}).exit_code,
           0);
  WriteFile(&f, "a.txt",
            Str8Lit("TOPCHANGE\nl02\nl03\nl04\nl05\nl06\nl07\nl08\nl09\nBOTCHANGE\n"));

  CommandExecLine(&f.ed, Str8Lit("git"));
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);
  View *view = EditorFocusedView(&f.ed);

  auto cursor_to = [&](String8 needle) {
    TempArena s = ScratchBegin();
    String8 t = BufferAll(s.arena, buffer);
    u64 at = Str8FindFirst(t, needle);
    CHECK(at < t.size);
    ViewSetCursorLineColumn(view, buffer, BufferLineFromOffset(buffer, at), 0);
    ScratchEnd(s);
  };

  // Expand the file to reveal its hunks.
  cursor_to(Str8Lit("a.txt"));
  CHECK(GitBufferToggleExpand(&f.ed, buffer, view));
  buffer = GitStatusBuffer(&f);
  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  CHECK(Contains(text, Str8Lit("TOPCHANGE")));
  CHECK(Contains(text, Str8Lit("BOTCHANGE")));
  CHECK(Str8FindFirst(text, Str8Lit("@@")) < text.size);
  ScratchEnd(scratch);

  // Fold the first hunk (the one containing TOPCHANGE): its body hides, the
  // second hunk is unaffected.
  cursor_to(Str8Lit("@@"));
  CHECK(GitBufferToggleExpand(&f.ed, buffer, view));
  buffer = GitStatusBuffer(&f);
  scratch = ScratchBegin();
  text = BufferAll(scratch.arena, buffer);
  CHECK(!Contains(text, Str8Lit("TOPCHANGE")));
  CHECK(Contains(text, Str8Lit("BOTCHANGE")));
  CHECK(Contains(text, Str8Lit("...")));
  ScratchEnd(scratch);

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
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("Commits")), (u32)TokenKind::Keyword);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("first commit")), (u32)TokenKind::Function);
  ScratchEnd(scratch);

  Destroy(&f);
}

TEST(git_diff_highlights_headers_and_changes) {
  Fixture f = MakeFixture("git_diff_highlight");
  String8 diff = Str8Lit(
      "diff --git a/a.txt b/a.txt\n"
      "index 111..222 100644\n"
      "--- a/a.txt\n"
      "+++ b/a.txt\n"
      "@@ -1 +1 @@\n"
      "-old line\n"
      "+new line\n"
      " unchanged");

  BufferHandle handle = GitBufferOpenDiff(&f.ed, Str8Lit("[diff]"), diff);
  Buffer *buffer = BufferFromHandle(&f.ed.buffers, handle);
  CHECK(buffer != nullptr);

  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("diff --git")), (u32)TokenKind::Keyword);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("@@")), (u32)TokenKind::Preprocessor);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("-old line")), (u32)TokenKind::Error);
  CHECK_EQ((u32)TokenKindAt(buffer, text, Str8Lit("+new line")), (u32)TokenKind::String);

  for (u64 i = 1; i < buffer->tokens.count; i += 1) {
    CHECK(buffer->tokens.tokens[i - 1].end <= buffer->tokens.tokens[i].start);
  }
  ScratchEnd(scratch);
  Destroy(&f);
}

TEST(git_diff_view_folds_hunk) {
  Fixture f = MakeFixture("git_diff_fold");
  String8 diff = Str8Lit(
      "commit deadbeef\n"
      "Author: Test <t@t.test>\n"
      "\n"
      "    subject\n"
      "\n"
      "diff --git a/a.txt b/a.txt\n"
      "index 111..222 100644\n"
      "--- a/a.txt\n"
      "+++ b/a.txt\n"
      "@@ -1,3 +1,3 @@\n"
      " alpha\n"
      "-ORIGTOP\n"
      "+NEWTOP\n"
      " gamma\n"
      "@@ -8,3 +8,3 @@\n"
      " hh\n"
      "-ORIGBOT\n"
      "+NEWBOT\n"
      " jj\n");

  BufferHandle handle = GitBufferOpenDiff(&f.ed, Str8Lit("[diff]"), diff);
  EditorShowBuffer(&f.ed, handle);
  Buffer *buffer = BufferFromHandle(&f.ed.buffers, handle);
  View *view = EditorFocusedView(&f.ed);
  CHECK(buffer != nullptr && view != nullptr);

  TempArena scratch = ScratchBegin();
  String8 text = BufferAll(scratch.arena, buffer);
  // The commit-header preamble survives, and both hunks are expanded.
  CHECK(Contains(text, Str8Lit("commit deadbeef")));
  CHECK(Contains(text, Str8Lit("NEWTOP")));
  CHECK(Contains(text, Str8Lit("NEWBOT")));
  ScratchEnd(scratch);

  // Fold the first hunk: cursor on its @@ header, then toggle.
  scratch = ScratchBegin();
  text = BufferAll(scratch.arena, buffer);
  u64 at = Str8FindFirst(text, Str8Lit("@@"));
  CHECK(at < text.size);
  ViewSetCursorLineColumn(view, buffer, BufferLineFromOffset(buffer, at), 0);
  ScratchEnd(scratch);
  CHECK(GitBufferToggleExpand(&f.ed, buffer, view));

  scratch = ScratchBegin();
  text = BufferAll(scratch.arena, buffer);
  CHECK(!Contains(text, Str8Lit("NEWTOP")));    // first hunk's body hidden
  CHECK(Contains(text, Str8Lit("NEWBOT")));     // second hunk untouched
  CHECK(Contains(text, Str8Lit("@@ -1,3 +1,3 @@ ...")));  // fold marker
  ScratchEnd(scratch);

  // Toggle again to unfold; the body comes back.
  CHECK(GitBufferToggleExpand(&f.ed, buffer, view));
  scratch = ScratchBegin();
  text = BufferAll(scratch.arena, buffer);
  CHECK(Contains(text, Str8Lit("NEWTOP")));
  CHECK(!Contains(text, Str8Lit("@@ -1,3 +1,3 @@ ...")));
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

TEST(git_gg_goes_to_file_start) {
  // Bare `g` used to be refresh and stole the first chord of `gg`.
  if (!HaveGit()) return;
  Fixture f = MakeFixture("git_gg");
  if (!InitRepo(&f)) {
    Destroy(&f);
    return;
  }
  WriteFile(&f, "a.txt", Str8Lit("x\n"));
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("add"), Str8Lit("a.txt")}).exit_code, 0);
  CHECK_EQ(RunIn(f.arena, f.dir.path, {Str8Lit("commit"), Str8Lit("-m"), Str8Lit("c")}).exit_code, 0);
  WriteFile(&f, "a.txt", Str8Lit("x\ny\n"));

  CommandExecLine(&f.ed, Str8Lit("git"));
  Buffer *buffer = GitStatusBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK(BufferLineCount(buffer) > 1);

  View *view = EditorFocusedView(&f.ed);
  ViewSetCursorLineColumn(view, buffer, BufferLineCount(buffer) - 1, 0);
  CHECK(ViewCursorLine(view, buffer) > 0);

  EditorProcessSpec(&f.ed, "gg");
  CHECK_EQ(ViewCursorLine(view, buffer), 0);

  Destroy(&f);
}
