#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// The explorer's listing, parsing and diff engine.
//
// Deliberately knows nothing about Buffer, View or Editor: it turns a directory
// into text, turns edited text back into a list of filesystem operations, and
// performs them. That keeps move detection -- the part most likely to be wrong
// -- testable from string literals with no editor to construct.
//
// The whole design rests on the rendered id. A line reads "003 main.cpp", and
// the id is what ties an edited line back to the entry it came from. Without it
// a rename is indistinguishable from a delete plus a create, which would lose
// the file's contents; with it, reordering lines is a no-op and moving a line
// into another directory is a move.

struct ExplorerEntry {
  u64 id;        // 1-based and unique within a snapshot; never 0
  String8 name;  // entry name only, no directory part
  bool is_dir;
  bool is_link;
};

struct ExplorerSnapshot {
  String8 dir;  // absolute, no trailing slash
  ExplorerEntry *entries;
  u64 count;
};

// Lists `dir` in the order OsDirList returns -- directories first, then names.
[[nodiscard]] ExplorerSnapshot ExplorerScan(Arena *arena, String8 dir);

// One line per entry: a %03llu id, one space, the name, then '/' for a
// directory and '@' for a symlink. Both suffixes are cosmetic and stripped on
// the way back in.
[[nodiscard]] String8 ExplorerRender(Arena *arena, const ExplorerSnapshot *snap);

struct ExplorerLine {
  u64 id;        // 0 when the line carries no id, which means "create this"
  String8 name;  // trimmed; a trailing '/' is kept, '@' is dropped
  bool blank;    // ignored entirely, so a stray blank line never deletes
};

// `max_id` is one past the largest id in the snapshot. Digits that fall outside
// that range are part of the name, so "12 Monkeys.mkv" in a three-entry
// directory is a create rather than a reference to id 12.
[[nodiscard]] ExplorerLine ExplorerParseLine(String8 line, u64 max_id);

enum class ExplorerOpKind : u8 {
  None = 0,
  Create,
  CreateDir,
  Move,
  Delete,
  DeleteDir,
  COUNT,
};

struct ExplorerOp {
  ExplorerOpKind kind;
  String8 from;  // absolute; empty for creates
  String8 to;    // absolute; empty for deletes
  // Set by ExplorerApply. Callers reacting to an operation -- pointing an open
  // buffer at a moved file, say -- must check this: a plan describes what was
  // asked for, not what happened.
  bool done;
};

struct ExplorerPlan {
  ExplorerOp *ops;
  u64 count;
  // Non-empty means the edit could not be understood and nothing should run.
  // Refusing beats guessing: every ambiguous case here is one where a wrong
  // guess destroys data.
  String8 error;
};

// Diffs `lines` against `snap`. Line order is never consulted, so sorting or
// reordering the buffer produces no operations at all.
[[nodiscard]] ExplorerPlan ExplorerDiff(Arena *arena, const ExplorerSnapshot *snap,
                                        String8 *lines, u64 line_count);

// Human-readable summary for the confirmation prompt.
[[nodiscard]] String8 ExplorerPlanSummary(Arena *arena, const ExplorerPlan *plan);

struct ExplorerApplyResult {
  u64 applied;
  u64 failed;
  String8 first_error;  // names the first operation that failed
};

// Runs the plan: deletes, then moves, then creates. Moves go via temporary
// names so that swapping two entries, or rotating a cycle of them, needs no
// special case. Keeps going after a failure and reports the count, since
// stopping halfway would leave a listing that matches neither state. Marks
// `done` on each operation that succeeded.
[[nodiscard]] ExplorerApplyResult ExplorerApply(Arena *arena, ExplorerPlan *plan);
