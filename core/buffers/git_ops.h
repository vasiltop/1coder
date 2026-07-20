#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Git porcelain helpers. Knows nothing about Buffer/View/Editor: it finds a
// repo, runs `git` via argv (never a shell), and parses status/diff/log text.
// That keeps the messy parts testable from string fixtures.

struct GitResult {
  bool started;
  i32 exit_code;
  String8 stdout_text;
  String8 stderr_text;
};

// Magit-style toggles that ride along with push/pull (and optionally commit).
struct GitFlags {
  bool rebase;        // pull --rebase
  bool autostash;     // pull --autostash
  bool ff_only;       // pull --ff-only
  bool set_upstream;  // push -u
};

enum class GitEntryKind : u8 {
  None = 0,
  Untracked,
  Unstaged,
  Staged,
  COUNT,
};

struct GitStatusEntry {
  GitEntryKind kind;
  String8 path;   // repo-relative, forward slashes
  String8 xy;     // two-char porcelain code, or "??"
  bool is_dir;    // untracked directory (path ends with /)
};

struct GitBranchInfo {
  String8 head;       // branch name or "HEAD" when detached
  String8 upstream;   // empty when none
  i32 ahead;
  i32 behind;
  bool detached;
};

struct GitStatus {
  GitBranchInfo branch;
  GitStatusEntry *entries;
  u64 count;
  String8 error;  // non-empty when status could not be read
};

struct GitHunk {
  String8 header;  // @@ -a,b +c,d @@ ...
  String8 body;    // lines including leading ' ', '+', '-', no trailing file trailer
  u64 old_start;
  u64 old_count;
  u64 new_start;
  u64 new_count;
};

struct GitDiffFile {
  String8 path;
  String8 header;  // diff --git ... through the @@ hunk start (exclusive)
  GitHunk *hunks;
  u64 hunk_count;
};

struct GitDiff {
  GitDiffFile *files;
  u64 file_count;
};

struct GitLogEntry {
  String8 hash;
  String8 subject;
};

struct GitLog {
  GitLogEntry *entries;
  u64 count;
};

// Walks up from `start` looking for `.git` (file or directory). Empty on miss.
[[nodiscard]] String8 GitFindRoot(Arena *arena, String8 start);

// True when `git` is on PATH.
[[nodiscard]] bool GitAvailable(Arena *arena);

// Runs `git <args...>` in `cwd`. Captures stdout and stderr.
[[nodiscard]] GitResult GitRun(Arena *arena, String8 cwd, const String8 *args, u64 arg_count);

// Appends active pull/push flags into `out` (does not clear it).
void GitFlagsAppendPull(Arena *arena, String8List *out, GitFlags flags);
void GitFlagsAppendPush(Arena *arena, String8List *out, GitFlags flags);

// Human-readable Args: line for the status header.
[[nodiscard]] String8 GitFlagsRender(Arena *arena, GitFlags flags);

// Parses porcelain=v1 -b output.
[[nodiscard]] GitStatus GitParseStatus(Arena *arena, String8 porcelain);

// Loads status for `root`.
[[nodiscard]] GitStatus GitLoadStatus(Arena *arena, String8 root);

// Parses a unified diff into files and hunks.
[[nodiscard]] GitDiff GitParseDiff(Arena *arena, String8 diff_text);

// `git diff` (worktree) or `git diff --cached` for one path, or all when path empty.
[[nodiscard]] GitDiff GitLoadDiff(Arena *arena, String8 root, String8 path, bool cached);

// Builds a complete patch for one hunk (file headers + hunk) suitable for
// `git apply --cached` / `git apply --cached --reverse`.
[[nodiscard]] String8 GitHunkPatch(Arena *arena, const GitDiffFile *file, const GitHunk *hunk);

[[nodiscard]] GitResult GitStagePath(Arena *arena, String8 root, String8 path);
[[nodiscard]] GitResult GitUnstagePath(Arena *arena, String8 root, String8 path);
[[nodiscard]] GitResult GitDiscardPath(Arena *arena, String8 root, String8 path);
[[nodiscard]] GitResult GitStageHunk(Arena *arena, String8 root, String8 patch);
[[nodiscard]] GitResult GitUnstageHunk(Arena *arena, String8 root, String8 patch);

[[nodiscard]] GitResult GitCommit(Arena *arena, String8 root, String8 message);
[[nodiscard]] GitResult GitPull(Arena *arena, String8 root, GitFlags flags);
[[nodiscard]] GitResult GitPush(Arena *arena, String8 root, GitFlags flags);

[[nodiscard]] GitLog GitLoadLog(Arena *arena, String8 root, u64 limit);
[[nodiscard]] String8 GitShow(Arena *arena, String8 root, String8 rev);
