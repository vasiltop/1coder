#include "buffers/git_ops.h"

#include "os/os_file.h"
#include "os/os_process.h"

namespace {

constexpr u64 kGitReadChunk = 4096;

String8 DrainStream(Arena *arena, OsProcess *process, bool stdout_stream) {
  String8List chunks = {};
  u8 buffer[kGitReadChunk];
  for (;;) {
    OsProcessRead read =
        stdout_stream ? OsProcessReadStdout(process, buffer, sizeof(buffer))
                      : OsProcessReadStderr(process, buffer, sizeof(buffer));
    if (read.status == OsProcessReadStatus::Data) {
      Str8ListPush(arena, &chunks, PushStr8Copy(arena, Str8(buffer, read.size)));
      continue;
    }
    break;
  }
  return Str8ListJoin(arena, &chunks, String8{});
}

bool PathHasGit(String8 path) {
  TempArena scratch = ScratchBegin();
  String8 marker = OsPathJoin(scratch.arena, path, Str8Lit(".git"));
  bool ok = OsDirExists(marker) || OsFileExists(marker);
  ScratchEnd(scratch);
  return ok;
}

i32 ParseI32(String8 s) {
  i32 value = 0;
  for (u64 i = 0; i < s.size; i += 1) {
    u8 c = s.str[i];
    if (c < '0' || c > '9') break;
    value = value * 10 + (i32)(c - '0');
  }
  return value;
}

u64 ParseU64(String8 s) {
  u64 value = 0;
  for (u64 i = 0; i < s.size; i += 1) {
    u8 c = s.str[i];
    if (c < '0' || c > '9') break;
    value = value * 10 + (u64)(c - '0');
  }
  return value;
}

void ParseBranchLine(Arena *arena, String8 line, GitBranchInfo *out) {
  // ## main...origin/main [ahead 1, behind 2]
  // ## HEAD (no branch)
  String8 rest = Str8Skip(line, 3);  // skip "## "
  rest = Str8SkipChopWhitespace(rest);

  if (Str8StartsWith(rest, Str8Lit("HEAD (no branch)"))) {
    out->detached = true;
    out->head = PushStr8Copy(arena, Str8Lit("HEAD"));
    return;
  }

  u64 dots = Str8FindFirst(rest, Str8Lit("..."));
  String8 head = (dots < rest.size) ? Str8Prefix(rest, dots) : rest;
  u64 head_space = Str8FindFirstChar(head, ' ');
  if (head_space < head.size) head = Str8Prefix(head, head_space);
  out->head = PushStr8Copy(arena, head);

  if (dots >= rest.size) return;

  String8 after = Str8Skip(rest, dots + 3);
  u64 bracket = Str8FindFirstChar(after, '[');
  String8 upstream = (bracket < after.size) ? Str8Prefix(after, bracket) : after;
  upstream = Str8SkipChopWhitespace(upstream);
  u64 up_space = Str8FindFirstChar(upstream, ' ');
  if (up_space < upstream.size) upstream = Str8Prefix(upstream, up_space);
  if (upstream.size > 0) out->upstream = PushStr8Copy(arena, upstream);

  if (bracket >= after.size) return;
  String8 tracking = Str8Skip(after, bracket + 1);
  if (tracking.size > 0 && tracking.str[tracking.size - 1] == ']') {
    tracking = Str8Chop(tracking, 1);
  }

  // Split on commas for "ahead N" / "behind M"
  String8List parts = Str8SplitChar(arena, tracking, ',');
  for (String8Node *node = parts.first; node; node = node->next) {
    String8 part = Str8SkipChopWhitespace(node->string);
    if (Str8StartsWith(part, Str8Lit("ahead "))) {
      out->ahead = ParseI32(Str8Skip(part, 6));
    } else if (Str8StartsWith(part, Str8Lit("behind "))) {
      out->behind = ParseI32(Str8Skip(part, 7));
    }
  }
}

bool ParseHunkHeader(String8 header, GitHunk *hunk) {
  // @@ -old_start,old_count +new_start,new_count @@
  if (!Str8StartsWith(header, Str8Lit("@@ "))) return false;
  u64 minus = Str8FindFirstChar(header, '-');
  if (minus >= header.size) return false;
  String8 old_part = Str8Skip(header, minus + 1);
  u64 space = Str8FindFirstChar(old_part, ' ');
  if (space >= old_part.size) return false;
  String8 old_range = Str8Prefix(old_part, space);
  String8 new_part = Str8Skip(old_part, space + 1);
  if (new_part.size == 0 || new_part.str[0] != '+') return false;
  new_part = Str8Skip(new_part, 1);
  u64 space2 = Str8FindFirstChar(new_part, ' ');
  String8 new_range = (space2 < new_part.size) ? Str8Prefix(new_part, space2) : new_part;

  auto parse_range = [](String8 range, u64 *start, u64 *count) {
    u64 comma = Str8FindFirstChar(range, ',');
    if (comma < range.size) {
      *start = ParseU64(Str8Prefix(range, comma));
      *count = ParseU64(Str8Skip(range, comma + 1));
    } else {
      *start = ParseU64(range);
      *count = 1;
    }
  };

  parse_range(old_range, &hunk->old_start, &hunk->old_count);
  parse_range(new_range, &hunk->new_start, &hunk->new_count);
  return true;
}

GitResult RunGit(Arena *arena, String8 cwd, String8List *argv) {
  GitResult result = {};
  if (!argv || argv->node_count == 0) return result;

  TempArena scratch = ScratchBegin1(arena);
  String8 git = OsFindExecutable(scratch.arena, Str8Lit("git"));
  if (git.size == 0) {
    result.stderr_text = PushStr8Copy(arena, Str8Lit("git not found on PATH"));
    ScratchEnd(scratch);
    return result;
  }

  String8 *args = PushArray(scratch.arena, String8, argv->node_count);
  u64 i = 0;
  for (String8Node *node = argv->first; node; node = node->next) {
    args[i] = node->string;
    i += 1;
  }

  OsProcessCommand cmd = {};
  cmd.executable = git;
  cmd.arguments = args;
  cmd.argument_count = argv->node_count;
  cmd.working_directory = cwd;

  OsProcess process = {};
  if (!OsProcessStart(&process, &cmd)) {
    result.stderr_text = PushStr8Copy(arena, Str8Lit("failed to start git"));
    ScratchEnd(scratch);
    return result;
  }

  OsProcessCloseStdin(&process);
  result.started = true;
  result.stdout_text = DrainStream(arena, &process, true);
  result.stderr_text = DrainStream(arena, &process, false);
  result.exit_code = OsProcessWait(&process);
  OsProcessDestroy(&process);
  ScratchEnd(scratch);
  return result;
}

GitResult RunGitArgs(Arena *arena, String8 cwd, const String8 *args, u64 arg_count) {
  TempArena scratch = ScratchBegin1(arena);
  String8List list = {};
  for (u64 i = 0; i < arg_count; i += 1) Str8ListPush(scratch.arena, &list, args[i]);
  GitResult result = RunGit(arena, cwd, &list);
  ScratchEnd(scratch);
  return result;
}

GitResult ApplyPatch(Arena *arena, String8 root, String8 patch, bool reverse) {
  TempArena scratch = ScratchBegin1(arena);
  String8List argv = {};
  Str8ListPush(scratch.arena, &argv, Str8Lit("apply"));
  Str8ListPush(scratch.arena, &argv, Str8Lit("--cached"));
  if (reverse) Str8ListPush(scratch.arena, &argv, Str8Lit("--reverse"));
  Str8ListPush(scratch.arena, &argv, Str8Lit("--whitespace=nowarn"));
  Str8ListPush(scratch.arena, &argv, Str8Lit("-"));

  // `git apply` reads the patch from stdin. OsProcessWrite then close.
  GitResult result = {};
  String8 git = OsFindExecutable(scratch.arena, Str8Lit("git"));
  if (git.size == 0) {
    result.stderr_text = PushStr8Copy(arena, Str8Lit("git not found on PATH"));
    ScratchEnd(scratch);
    return result;
  }

  String8 *args = PushArray(scratch.arena, String8, argv.node_count);
  u64 i = 0;
  for (String8Node *node = argv.first; node; node = node->next) {
    args[i] = node->string;
    i += 1;
  }

  OsProcessCommand cmd = {};
  cmd.executable = git;
  cmd.arguments = args;
  cmd.argument_count = argv.node_count;
  cmd.working_directory = root;

  OsProcess process = {};
  if (!OsProcessStart(&process, &cmd)) {
    result.stderr_text = PushStr8Copy(arena, Str8Lit("failed to start git"));
    ScratchEnd(scratch);
    return result;
  }

  result.started = true;
  (void)OsProcessWrite(&process, patch);
  OsProcessCloseStdin(&process);
  result.stdout_text = DrainStream(arena, &process, true);
  result.stderr_text = DrainStream(arena, &process, false);
  result.exit_code = OsProcessWait(&process);
  OsProcessDestroy(&process);
  ScratchEnd(scratch);
  return result;
}

}  // namespace

String8 GitFindRoot(Arena *arena, String8 start) {
  if (start.size == 0) return String8{};

  TempArena scratch = ScratchBegin1(arena);
  String8 absolute = OsPathAbsolute(scratch.arena, start);
  if (OsFileExists(absolute) && !OsDirExists(absolute)) {
    absolute = Str8PathDir(absolute);
  }

  String8 cur = absolute;
  for (;;) {
    if (PathHasGit(cur)) {
      String8 root = PushStr8Copy(arena, cur);
      ScratchEnd(scratch);
      return root;
    }
    if (Str8PathIsRoot(cur)) break;
    String8 parent = Str8PathDir(cur);
    if (parent.size == 0 || Str8Match(parent, cur)) break;
    cur = parent;
  }

  ScratchEnd(scratch);
  return String8{};
}

bool GitAvailable(Arena *arena) {
  TempArena scratch = ScratchBegin1(arena);
  String8 git = OsFindExecutable(scratch.arena, Str8Lit("git"));
  bool ok = git.size > 0;
  ScratchEnd(scratch);
  return ok;
}

GitResult GitRun(Arena *arena, String8 cwd, const String8 *args, u64 arg_count) {
  return RunGitArgs(arena, cwd, args, arg_count);
}

void GitFlagsAppendPull(Arena *arena, String8List *out, GitFlags flags) {
  if (!out) return;
  if (flags.rebase) Str8ListPush(arena, out, Str8Lit("--rebase"));
  if (flags.autostash) Str8ListPush(arena, out, Str8Lit("--autostash"));
  if (flags.ff_only) Str8ListPush(arena, out, Str8Lit("--ff-only"));
}

void GitFlagsAppendPush(Arena *arena, String8List *out, GitFlags flags) {
  if (!out) return;
  if (flags.set_upstream) Str8ListPush(arena, out, Str8Lit("-u"));
}

String8 GitFlagsRender(Arena *arena, GitFlags flags) {
  String8List parts = {};
  if (flags.rebase) Str8ListPush(arena, &parts, Str8Lit("--rebase"));
  if (flags.autostash) Str8ListPush(arena, &parts, Str8Lit("--autostash"));
  if (flags.ff_only) Str8ListPush(arena, &parts, Str8Lit("--ff-only"));
  if (flags.set_upstream) Str8ListPush(arena, &parts, Str8Lit("-u"));
  if (parts.node_count == 0) return PushStr8Copy(arena, Str8Lit("none"));
  return Str8ListJoin(arena, &parts, Str8Lit(" "));
}

GitStatus GitParseStatus(Arena *arena, String8 porcelain) {
  GitStatus status = {};
  String8List lines = Str8SplitChar(arena, porcelain, '\n');

  // First pass: count entries.
  u64 entry_count = 0;
  for (String8Node *node = lines.first; node; node = node->next) {
    String8 line = node->string;
    if (line.size == 0) continue;
    if (line.size >= 2 && line.str[0] == '#' && line.str[1] == '#') continue;
    entry_count += 1;
  }

  status.entries = PushArray(arena, GitStatusEntry, Max(entry_count * 2, (u64)1));
  u64 out = 0;

  for (String8Node *node = lines.first; node; node = node->next) {
    String8 line = node->string;
    if (line.size == 0) continue;

    if (line.size >= 2 && line.str[0] == '#' && line.str[1] == '#') {
      ParseBranchLine(arena, line, &status.branch);
      continue;
    }

    if (line.size < 3) continue;

    String8 xy = Str8Prefix(line, 2);
    String8 path = Str8Skip(line, 3);

    // Rename: "R  old -> new" — take the new path.
    u64 arrow = Str8FindFirst(path, Str8Lit(" -> "));
    if (arrow < path.size) path = Str8Skip(path, arrow + 4);

    path = Str8SkipChopWhitespace(path);
    if (path.size == 0) continue;

    u8 x = xy.str[0];
    u8 y = xy.str[1];

    if (x == '?' && y == '?') {
      GitStatusEntry *e = &status.entries[out++];
      e->kind = GitEntryKind::Untracked;
      e->path = PushStr8Copy(arena, path);
      e->xy = PushStr8Copy(arena, xy);
      e->is_dir = path.size > 0 && path.str[path.size - 1] == '/';
      continue;
    }

    // Staged when index side is set.
    if (x != ' ' && x != '?') {
      GitStatusEntry *e = &status.entries[out++];
      e->kind = GitEntryKind::Staged;
      e->path = PushStr8Copy(arena, path);
      e->xy = PushStr8Copy(arena, xy);
    }

    // Unstaged when worktree side is set.
    if (y != ' ' && y != '?') {
      GitStatusEntry *e = &status.entries[out++];
      e->kind = GitEntryKind::Unstaged;
      e->path = PushStr8Copy(arena, path);
      e->xy = PushStr8Copy(arena, xy);
    }
  }

  status.count = out;
  if (status.branch.head.size == 0) {
    status.branch.head = PushStr8Copy(arena, Str8Lit("HEAD"));
  }
  return status;
}

GitStatus GitLoadStatus(Arena *arena, String8 root) {
  GitStatus empty = {};
  if (root.size == 0) {
    empty.error = PushStr8Copy(arena, Str8Lit("not a git repository"));
    return empty;
  }

  String8 args[] = {Str8Lit("status"), Str8Lit("--porcelain=v1"), Str8Lit("-b")};
  GitResult result = RunGitArgs(arena, root, args, ArrayCount(args));
  if (!result.started) {
    empty.error = result.stderr_text.size ? result.stderr_text
                                          : PushStr8Copy(arena, Str8Lit("git not available"));
    return empty;
  }
  if (result.exit_code != 0 && result.stdout_text.size == 0) {
    empty.error = result.stderr_text.size ? result.stderr_text
                                          : PushStr8Copy(arena, Str8Lit("git status failed"));
    return empty;
  }
  return GitParseStatus(arena, result.stdout_text);
}

GitDiff GitParseDiff(Arena *arena, String8 diff_text) {
  GitDiff diff = {};
  if (diff_text.size == 0) return diff;

  String8List lines = Str8SplitChar(arena, diff_text, '\n');

  // Count files and hunks for allocation.
  u64 file_count = 0;
  u64 hunk_count = 0;
  for (String8Node *node = lines.first; node; node = node->next) {
    if (Str8StartsWith(node->string, Str8Lit("diff --git "))) file_count += 1;
    if (Str8StartsWith(node->string, Str8Lit("@@ "))) hunk_count += 1;
  }

  diff.files = PushArray(arena, GitDiffFile, Max(file_count, (u64)1));
  GitHunk *hunk_pool = PushArray(arena, GitHunk, Max(hunk_count, (u64)1));
  u64 file_i = 0;
  u64 hunk_i = 0;

  GitDiffFile *cur = nullptr;
  String8List header_lines = {};
  String8List hunk_body = {};
  GitHunk *cur_hunk = nullptr;
  bool in_hunk = false;

  auto finish_hunk = [&]() {
    if (!cur || !cur_hunk) return;
    cur_hunk->body = Str8ListJoin(arena, &hunk_body, Str8Lit("\n"));
    if (hunk_body.node_count > 0) {
      // Re-join with newlines; trailing newline is fine for apply.
      cur_hunk->body = PushStr8Cat(arena, cur_hunk->body, Str8Lit("\n"));
    }
    hunk_body = {};
    cur_hunk = nullptr;
    in_hunk = false;
  };

  auto finish_file = [&]() {
    finish_hunk();
    if (!cur) return;
    cur->header = Str8ListJoin(arena, &header_lines, Str8Lit("\n"));
    if (header_lines.node_count > 0) {
      cur->header = PushStr8Cat(arena, cur->header, Str8Lit("\n"));
    }
    header_lines = {};
    cur = nullptr;
  };

  for (String8Node *node = lines.first; node; node = node->next) {
    String8 line = node->string;

    if (Str8StartsWith(line, Str8Lit("diff --git "))) {
      finish_file();
      cur = &diff.files[file_i++];
      cur->hunks = &hunk_pool[hunk_i];
      cur->hunk_count = 0;

      // path: "diff --git a/foo b/foo" — take b/ side.
      u64 b_pos = Str8FindFirst(line, Str8Lit(" b/"));
      if (b_pos < line.size) {
        cur->path = PushStr8Copy(arena, Str8Skip(line, b_pos + 3));
      }
      Str8ListPush(arena, &header_lines, line);
      continue;
    }

    if (!cur) continue;

    if (Str8StartsWith(line, Str8Lit("@@ "))) {
      finish_hunk();
      cur_hunk = &hunk_pool[hunk_i++];
      cur->hunk_count += 1;
      cur_hunk->header = PushStr8Copy(arena, line);
      ParseHunkHeader(line, cur_hunk);
      in_hunk = true;
      continue;
    }

    if (in_hunk) {
      // End of hunk content markers aren't present; next @@ or diff ends it.
      // Skip "\\ No newline at end of file"
      if (Str8StartsWith(line, Str8Lit("\\ "))) {
        Str8ListPush(arena, &hunk_body, line);
        continue;
      }
      if (line.size == 0 || line.str[0] == ' ' || line.str[0] == '+' || line.str[0] == '-') {
        Str8ListPush(arena, &hunk_body, line);
        continue;
      }
      // Unexpected — treat as end of hunk.
      finish_hunk();
      Str8ListPush(arena, &header_lines, line);
      continue;
    }

    Str8ListPush(arena, &header_lines, line);
  }

  finish_file();
  diff.file_count = file_i;
  return diff;
}

GitDiff GitLoadDiff(Arena *arena, String8 root, String8 path, bool cached) {
  TempArena scratch = ScratchBegin1(arena);
  String8List argv = {};
  Str8ListPush(scratch.arena, &argv, Str8Lit("diff"));
  if (cached) Str8ListPush(scratch.arena, &argv, Str8Lit("--cached"));
  Str8ListPush(scratch.arena, &argv, Str8Lit("--"));
  if (path.size > 0) Str8ListPush(scratch.arena, &argv, path);

  GitResult result = RunGit(arena, root, &argv);
  ScratchEnd(scratch);
  if (!result.started) return GitDiff{};
  return GitParseDiff(arena, result.stdout_text);
}

String8 GitHunkPatch(Arena *arena, const GitDiffFile *file, const GitHunk *hunk) {
  if (!file || !hunk) return String8{};
  String8List parts = {};
  Str8ListPush(arena, &parts, file->header);
  Str8ListPush(arena, &parts, hunk->header);
  Str8ListPush(arena, &parts, Str8Lit("\n"));
  Str8ListPush(arena, &parts, hunk->body);
  return Str8ListJoin(arena, &parts, String8{});
}

GitResult GitStagePath(Arena *arena, String8 root, String8 path) {
  String8 args[] = {Str8Lit("add"), Str8Lit("--"), path};
  return RunGitArgs(arena, root, args, ArrayCount(args));
}

GitResult GitUnstagePath(Arena *arena, String8 root, String8 path) {
  String8 args[] = {Str8Lit("restore"), Str8Lit("--staged"), Str8Lit("--"), path};
  return RunGitArgs(arena, root, args, ArrayCount(args));
}

GitResult GitDiscardPath(Arena *arena, String8 root, String8 path) {
  // Untracked: remove. Tracked: restore worktree.
  String8 status_args[] = {Str8Lit("status"), Str8Lit("--porcelain=v1"), Str8Lit("--"), path};
  GitResult st = RunGitArgs(arena, root, status_args, ArrayCount(status_args));
  if (st.started && Str8StartsWith(Str8SkipChopWhitespace(st.stdout_text), Str8Lit("??"))) {
    String8 args[] = {Str8Lit("clean"), Str8Lit("-f"), Str8Lit("--"), path};
    return RunGitArgs(arena, root, args, ArrayCount(args));
  }
  String8 args[] = {Str8Lit("restore"), Str8Lit("--"), path};
  return RunGitArgs(arena, root, args, ArrayCount(args));
}

GitResult GitStageHunk(Arena *arena, String8 root, String8 patch) {
  return ApplyPatch(arena, root, patch, false);
}

GitResult GitUnstageHunk(Arena *arena, String8 root, String8 patch) {
  return ApplyPatch(arena, root, patch, true);
}

GitResult GitCommit(Arena *arena, String8 root, String8 message) {
  String8 args[] = {Str8Lit("commit"), Str8Lit("-m"), message};
  return RunGitArgs(arena, root, args, ArrayCount(args));
}

GitResult GitPull(Arena *arena, String8 root, GitFlags flags) {
  TempArena scratch = ScratchBegin1(arena);
  String8List argv = {};
  Str8ListPush(scratch.arena, &argv, Str8Lit("pull"));
  GitFlagsAppendPull(scratch.arena, &argv, flags);
  GitResult result = RunGit(arena, root, &argv);
  ScratchEnd(scratch);
  return result;
}

GitResult GitPush(Arena *arena, String8 root, GitFlags flags) {
  TempArena scratch = ScratchBegin1(arena);
  String8List argv = {};
  Str8ListPush(scratch.arena, &argv, Str8Lit("push"));
  GitFlagsAppendPush(scratch.arena, &argv, flags);
  if (flags.set_upstream) {
    // git push -u origin HEAD when setting upstream for a new branch
    Str8ListPush(scratch.arena, &argv, Str8Lit("origin"));
    Str8ListPush(scratch.arena, &argv, Str8Lit("HEAD"));
  }
  GitResult result = RunGit(arena, root, &argv);
  ScratchEnd(scratch);
  return result;
}

GitLog GitLoadLog(Arena *arena, String8 root, u64 limit) {
  GitLog log = {};
  TempArena scratch = ScratchBegin1(arena);
  String8 limit_str = PushStr8F(scratch.arena, "%llu", (unsigned long long)limit);
  String8 args[] = {Str8Lit("log"), Str8Lit("--oneline"), Str8Lit("-n"), limit_str};
  GitResult result = RunGitArgs(arena, root, args, ArrayCount(args));
  ScratchEnd(scratch);
  if (!result.started || result.exit_code != 0) return log;

  String8List lines = Str8SplitChar(arena, result.stdout_text, '\n');
  u64 count = 0;
  for (String8Node *node = lines.first; node; node = node->next) {
    if (Str8SkipChopWhitespace(node->string).size > 0) count += 1;
  }
  log.entries = PushArray(arena, GitLogEntry, Max(count, (u64)1));
  for (String8Node *node = lines.first; node; node = node->next) {
    String8 line = Str8SkipChopWhitespace(node->string);
    if (line.size == 0) continue;
    u64 space = Str8FindFirstChar(line, ' ');
    GitLogEntry *e = &log.entries[log.count++];
    if (space < line.size) {
      e->hash = PushStr8Copy(arena, Str8Prefix(line, space));
      e->subject = PushStr8Copy(arena, Str8Skip(line, space + 1));
    } else {
      e->hash = PushStr8Copy(arena, line);
      e->subject = String8{};
    }
  }
  return log;
}

String8 GitShow(Arena *arena, String8 root, String8 rev) {
  String8 args[] = {Str8Lit("show"), Str8Lit("--stat"), Str8Lit("--patch"), rev};
  GitResult result = RunGitArgs(arena, root, args, ArrayCount(args));
  if (!result.started) return result.stderr_text;
  if (result.exit_code != 0) {
    return result.stderr_text.size ? result.stderr_text : result.stdout_text;
  }
  return result.stdout_text;
}
