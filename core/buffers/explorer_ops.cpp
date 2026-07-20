#include "buffers/explorer_ops.h"

#include "os/os_file.h"

// ---------------------------------------------------------------------------
// Scan and render
// ---------------------------------------------------------------------------

ExplorerSnapshot ExplorerScan(Arena *arena, String8 dir) {
  ExplorerSnapshot snap = {};
  snap.dir = PushStr8Copy(arena, dir);

  FileList list = OsDirList(arena, dir);
  snap.entries = PushArray(arena, ExplorerEntry, Max(list.count, (u64)1));
  snap.count = list.count;

  for (u64 i = 0; i < list.count; i += 1) {
    ExplorerEntry *entry = &snap.entries[i];
    entry->id = i + 1;  // 1-based, so 0 stays available to mean "no id"
    entry->name = list.files[i].name;
    entry->is_dir = list.files[i].is_dir;
    entry->is_link = list.files[i].is_link;
  }

  return snap;
}

String8 ExplorerRender(Arena *arena, const ExplorerSnapshot *snap) {
  TempArena scratch = ScratchBegin1(arena);

  String8List lines = {};
  for (u64 i = 0; i < snap->count; i += 1) {
    ExplorerEntry *entry = &snap->entries[i];
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%03llu %.*s%s%s", (unsigned long long)entry->id,
                           (int)entry->name.size, (char *)entry->name.str,
                           entry->is_dir ? "/" : "", entry->is_link ? "@" : ""));
  }

  String8 text = Str8ListJoin(arena, &lines, Str8Lit("\n"));
  ScratchEnd(scratch);
  return text;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

ExplorerLine ExplorerParseLine(String8 line, u64 max_id) {
  ExplorerLine parsed = {};

  String8 trimmed = Str8SkipChopWhitespace(line);
  if (trimmed.size == 0) {
    parsed.blank = true;
    return parsed;
  }

  // An id is leading digits followed by exactly one space, and only when the
  // value actually names an entry. Anything else is part of the name.
  u64 digits = 0;
  while (digits < trimmed.size && CharIsDigit(trimmed.str[digits])) digits += 1;

  if (digits > 0 && digits < trimmed.size && trimmed.str[digits] == ' ') {
    u64 value = 0;
    bool overflow = false;
    for (u64 i = 0; i < digits; i += 1) {
      if (value > (u64)1 << 40) {  // far past any plausible entry count
        overflow = true;
        break;
      }
      value = value * 10 + (u64)(trimmed.str[i] - '0');
    }

    if (!overflow && value >= 1 && value < max_id) {
      parsed.id = value;
      parsed.name = Str8SkipChopWhitespace(Str8Skip(trimmed, digits + 1));
    }
  }

  if (parsed.id == 0) parsed.name = trimmed;

  // '@' is decoration the renderer added; a name genuinely ending in '@' is
  // rare enough that keeping the marker round-trippable is the better trade.
  if (parsed.name.size > 0 && parsed.name.str[parsed.name.size - 1] == '@') {
    parsed.name = Str8Chop(parsed.name, 1);
  }

  if (parsed.name.size == 0) parsed.blank = true;
  return parsed;
}

namespace {

[[nodiscard]] bool NameIsDir(String8 name) {
  return name.size > 0 && name.str[name.size - 1] == '/';
}

[[nodiscard]] String8 StripTrailingSlash(String8 name) {
  while (name.size > 0 && name.str[name.size - 1] == '/') name = Str8Chop(name, 1);
  return name;
}

// A name is only allowed to describe something inside the listed directory.
// "." and ".." would let an edit reach outside it, and an absolute path would
// ignore it entirely.
[[nodiscard]] bool NameIsSafe(Arena *scratch_arena, String8 name) {
  if (name.size == 0) return false;
  if (name.str[0] == '/') return false;

  String8List parts = Str8SplitChar(scratch_arena, StripTrailingSlash(name), '/');
  for (String8Node *node = parts.first; node; node = node->next) {
    if (Str8Match(node->string, Str8Lit(".")) || Str8Match(node->string, Str8Lit(".."))) {
      return false;
    }
  }
  return true;
}

struct OpList {
  Arena *arena;
  ExplorerOp *ops;
  u64 count;
  u64 capacity;
};

void OpPush(OpList *list, ExplorerOpKind kind, String8 from, String8 to) {
  if (list->count >= list->capacity) return;
  ExplorerOp *op = &list->ops[list->count++];
  op->kind = kind;
  op->from = from;
  op->to = to;
}

}  // namespace

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

ExplorerPlan ExplorerDiff(Arena *arena, const ExplorerSnapshot *snap, String8 *lines,
                          u64 line_count) {
  ExplorerPlan plan = {};

  TempArena scratch = ScratchBegin1(arena);

  // One slot per snapshot entry, holding the line that claimed its id. Order is
  // never consulted, which is precisely what makes reordering a no-op.
  String8 *claimed = PushArray(scratch.arena, String8, Max(snap->count, (u64)1));
  bool *seen = PushArray(scratch.arena, bool, Max(snap->count, (u64)1));

  // Worst case every line is a create and every entry a delete.
  OpList ops = {};
  ops.arena = arena;
  ops.capacity = line_count + snap->count + 1;
  ops.ops = PushArray(arena, ExplorerOp, ops.capacity);

  u64 max_id = snap->count + 1;

  // Pass one: bind ids to lines, collecting creates as we go.
  for (u64 i = 0; i < line_count; i += 1) {
    ExplorerLine parsed = ExplorerParseLine(lines[i], max_id);
    if (parsed.blank) continue;

    if (!NameIsSafe(scratch.arena, parsed.name)) {
      plan.error = PushStr8F(arena, "invalid name \"%.*s\"", (int)parsed.name.size,
                             (char *)parsed.name.str);
      ScratchEnd(scratch);
      return plan;
    }

    if (parsed.id == 0) {
      String8 target = OsPathJoin(arena, snap->dir, StripTrailingSlash(parsed.name));
      OpPush(&ops, NameIsDir(parsed.name) ? ExplorerOpKind::CreateDir : ExplorerOpKind::Create,
             String8{nullptr, 0}, target);
      continue;
    }

    u64 slot = parsed.id - 1;
    if (seen[slot]) {
      // What `yyp` produces. Copy and rename are both plausible readings and
      // guessing wrong either duplicates or destroys, so say so instead.
      plan.error = PushStr8F(arena, "duplicate id %03llu (clear the id to create a copy)",
                             (unsigned long long)parsed.id);
      ScratchEnd(scratch);
      return plan;
    }
    seen[slot] = true;
    claimed[slot] = StripTrailingSlash(parsed.name);
  }

  // Pass two: an entry whose id no line claimed is a delete; one whose name
  // changed is a move. An unchanged name produces nothing.
  for (u64 i = 0; i < snap->count; i += 1) {
    ExplorerEntry *entry = &snap->entries[i];
    String8 from = OsPathJoin(arena, snap->dir, entry->name);

    if (!seen[i]) {
      // A symlink to a directory is unlinked, not walked, so it deletes as a
      // file however its target looks.
      bool as_dir = entry->is_dir && !entry->is_link;
      OpPush(&ops, as_dir ? ExplorerOpKind::DeleteDir : ExplorerOpKind::Delete, from,
             String8{nullptr, 0});
      continue;
    }

    if (Str8Match(claimed[i], entry->name)) continue;

    OpPush(&ops, ExplorerOpKind::Move, from, OsPathJoin(arena, snap->dir, claimed[i]));
  }

  ScratchEnd(scratch);

  plan.ops = ops.ops;
  plan.count = ops.count;
  return plan;
}

String8 ExplorerPlanSummary(Arena *arena, const ExplorerPlan *plan) {
  TempArena scratch = ScratchBegin1(arena);

  u64 creates = 0, moves = 0, deletes = 0;
  for (u64 i = 0; i < plan->count; i += 1) {
    switch (plan->ops[i].kind) {
      case ExplorerOpKind::Create:
      case ExplorerOpKind::CreateDir: creates += 1; break;
      case ExplorerOpKind::Move: moves += 1; break;
      case ExplorerOpKind::Delete:
      case ExplorerOpKind::DeleteDir: deletes += 1; break;
      default: break;
    }
  }

  String8List parts = {};
  if (creates) {
    Str8ListPush(scratch.arena, &parts,
                 PushStr8F(scratch.arena, "%llu to create", (unsigned long long)creates));
  }
  if (moves) {
    Str8ListPush(scratch.arena, &parts,
                 PushStr8F(scratch.arena, "%llu to move", (unsigned long long)moves));
  }
  if (deletes) {
    // Named last and first in the reader's mind: it is the irreversible one.
    Str8ListPush(scratch.arena, &parts,
                 PushStr8F(scratch.arena, "%llu to DELETE", (unsigned long long)deletes));
  }

  String8 summary = Str8ListJoin(arena, &parts, Str8Lit(", "));
  ScratchEnd(scratch);
  return summary;
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

namespace {

void RecordFailure(ExplorerApplyResult *result, Arena *arena, const char *verb, String8 path) {
  result->failed += 1;
  if (result->first_error.size == 0) {
    result->first_error =
        PushStr8F(arena, "%s failed: %.*s", verb, (int)path.size, (char *)path.str);
  }
}

}  // namespace

ExplorerApplyResult ExplorerApply(Arena *arena, const ExplorerPlan *plan) {
  ExplorerApplyResult result = {};
  if (plan->error.size != 0) return result;

  TempArena scratch = ScratchBegin1(arena);

  // Deletes first, so that renaming onto a name being freed in the same edit
  // works rather than colliding.
  for (u64 i = 0; i < plan->count; i += 1) {
    ExplorerOp *op = &plan->ops[i];
    if (op->kind == ExplorerOpKind::Delete) {
      if (OsFileDelete(op->from)) result.applied += 1;
      else RecordFailure(&result, arena, "delete", op->from);
    } else if (op->kind == ExplorerOpKind::DeleteDir) {
      if (OsDirDeleteRecursive(op->from)) result.applied += 1;
      else RecordFailure(&result, arena, "delete", op->from);
    }
  }

  // Moves in two phases. Sending every source to a unique temporary first means
  // a swap (a->b, b->a) and a rotation (a->b->c->a) both work with no cycle
  // detection at all -- by the time any final name is written, every name the
  // edit touched has already been vacated.
  u64 move_count = 0;
  for (u64 i = 0; i < plan->count; i += 1) {
    if (plan->ops[i].kind == ExplorerOpKind::Move) move_count += 1;
  }

  String8 *staged = PushArray(scratch.arena, String8, Max(move_count, (u64)1));
  ExplorerOp **moves = PushArray(scratch.arena, ExplorerOp *, Max(move_count, (u64)1));
  u64 staged_count = 0;

  for (u64 i = 0; i < plan->count; i += 1) {
    ExplorerOp *op = &plan->ops[i];
    if (op->kind != ExplorerOpKind::Move) continue;

    String8 temp = PushStr8F(scratch.arena, "%.*s.1code-move-%llu", (int)op->to.size,
                             (char *)op->to.str, (unsigned long long)staged_count);
    if (!OsRename(op->from, temp)) {
      RecordFailure(&result, arena, "move", op->from);
      continue;
    }
    staged[staged_count] = temp;
    moves[staged_count] = op;
    staged_count += 1;
  }

  for (u64 i = 0; i < staged_count; i += 1) {
    if (OsRename(staged[i], moves[i]->to)) {
      result.applied += 1;
    } else {
      // Put it back rather than leaving a file under a temporary name.
      bool restored = OsRename(staged[i], moves[i]->from);
      (void)restored;
      RecordFailure(&result, arena, "move", moves[i]->from);
    }
  }

  // Creates last, so a new file can take a name that a move or delete freed.
  for (u64 i = 0; i < plan->count; i += 1) {
    ExplorerOp *op = &plan->ops[i];
    if (op->kind == ExplorerOpKind::Create) {
      if (OsFileCreate(op->to)) result.applied += 1;
      else RecordFailure(&result, arena, "create", op->to);
    } else if (op->kind == ExplorerOpKind::CreateDir) {
      if (OsMakeDirs(op->to)) result.applied += 1;
      else RecordFailure(&result, arena, "create", op->to);
    }
  }

  ScratchEnd(scratch);
  return result;
}
