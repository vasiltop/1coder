#include "buffers/buf_explorer.h"

#include "buffers/explorer_ops.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"

// The explorer buffer. Everything specific to it lives here: core knows only
// that some buffer kind has hooks.
//
// It is deliberately *not* read-only. Grep results are a view of something
// else, but a directory listing is the thing itself, and editing it is the
// whole feature.

namespace {

struct ExplorerPayload {
  String8 dir;  // absolute, no trailing slash

  // The listing as it was last read from disk. Every diff is against this, not
  // against the previous edit, so ids stay meaningful across a whole session of
  // editing before the write.
  ExplorerSnapshot snapshot;
  Arena *snapshot_arena;  // cleared and refilled on each reload

  // The plan the pending confirmation would run. Stored between :w and the y/n
  // keystroke so the answer does not have to re-diff a buffer the user may have
  // touched in between.
  ExplorerPlan pending;
  Arena *pending_arena;

  bool reloading;  // guards against a reload being treated as a user edit
};

[[nodiscard]] ExplorerPayload *PayloadFor(const Buffer *buffer) {
  if (!buffer || buffer->kind != BufferKind::Explorer) return nullptr;
  return (ExplorerPayload *)buffer->user_data;
}

void ExplorerOnClose(Editor *ed, Buffer *buffer) {
  (void)ed;
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload) return;

  if (payload->snapshot_arena) ArenaRelease(payload->snapshot_arena);
  if (payload->pending_arena) ArenaRelease(payload->pending_arena);
  payload->snapshot_arena = nullptr;
  payload->pending_arena = nullptr;
}

// Refocusing an explorer picks up whatever changed on disk while you were away,
// which is what makes it feel live. Unsaved edits win, since silently throwing
// away a half-typed rename would be worse than showing a stale listing.
void ExplorerOnActivate(Editor *ed, Buffer *buffer, View *view) {
  (void)view;
  if (BufferIsDirty(buffer)) return;
  ExplorerBufferReload(ed, buffer);
}

bool ExplorerOnWrite(Editor *ed, Buffer *buffer, View *view);

}  // namespace

// ---------------------------------------------------------------------------
// Opening and reloading
// ---------------------------------------------------------------------------

void ExplorerBufferReload(Editor *ed, Buffer *buffer) {
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload || payload->reloading) return;

  payload->reloading = true;

  ArenaClear(payload->snapshot_arena);
  payload->snapshot = ExplorerScan(payload->snapshot_arena, payload->dir);

  TempArena scratch = ScratchBegin();
  // BufferSetText rather than BufferReplace: a reload is a load, so it clears
  // undo and the dirty flag. Keeping undo across a reload would let `u` restore
  // a listing whose ids no longer describe anything on disk.
  BufferSetText(ed, buffer, ExplorerRender(scratch.arena, &payload->snapshot));
  ScratchEnd(scratch);

  payload->reloading = false;
}

BufferHandle ExplorerBufferOpen(Editor *ed, String8 dir) {
  TempArena scratch = ScratchBegin1(ed->arena);
  String8 absolute = OsPathAbsolute(scratch.arena, dir);

  // Trailing separators would defeat the dedupe: "/src" and "/src/" must be the
  // same buffer. The root is left alone, since "" is not a directory.
  while (absolute.size > 1 && absolute.str[absolute.size - 1] == '/') {
    absolute = Str8Chop(absolute, 1);
  }

  if (!OsDirExists(absolute)) {
    ScratchEnd(scratch);
    return BufferHandleZero();
  }

  BufferHandle existing = BufferFromPath(&ed->buffers, absolute);
  if (existing.index != 0) {
    Buffer *buffer = BufferFromHandle(&ed->buffers, existing);
    if (buffer && buffer->kind == BufferKind::Explorer) {
      ScratchEnd(scratch);
      return existing;
    }
  }

  // The name carries a trailing slash so the status line reads "src/" and an
  // explorer is never mistaken for a file of the same name.
  String8 name = PushStr8F(scratch.arena, "%.*s/", (int)Str8PathBase(absolute).size,
                           (char *)Str8PathBase(absolute).str);

  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::Explorer, name);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) {
    ScratchEnd(scratch);
    return BufferHandleZero();
  }

  buffer->path = PushStr8Copy(buffer->arena, absolute);

  ExplorerPayload *payload = PushStruct(buffer->arena, ExplorerPayload);
  payload->dir = PushStr8Copy(buffer->arena, absolute);
  payload->snapshot_arena = ArenaAlloc(MB(16));
  payload->pending_arena = ArenaAlloc(MB(4));

  buffer->user_data = payload;
  buffer->hooks.on_activate = ExplorerOnActivate;
  buffer->hooks.on_close = ExplorerOnClose;
  buffer->hooks.on_write = ExplorerOnWrite;

  // Only two keys are claimed. Everything else -- motions, operators, insert
  // mode, `:` -- keeps working, because a buffer-local map layers above the
  // active mode map rather than replacing it. That is what makes editing the
  // listing feel like editing any other buffer.
  Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
  KeymapBind(keymap, "<CR>", CommandId::explorer_open);
  KeymapBind(keymap, "-", CommandId::explorer_parent);
  buffer->hooks.keymap = keymap;

  ScratchEnd(scratch);

  ExplorerBufferReload(ed, buffer);
  return handle;
}

String8 ExplorerBufferDir(const Buffer *buffer) {
  ExplorerPayload *payload = PayloadFor(buffer);
  return payload ? payload->dir : String8{nullptr, 0};
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

void ExplorerBufferFocusName(Editor *ed, Buffer *buffer, View *view, String8 name) {
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload || !view || name.size == 0) return;

  for (u64 i = 0; i < payload->snapshot.count; i += 1) {
    if (!Str8Match(payload->snapshot.entries[i].name, name)) continue;
    ViewSetCursorLineColumn(view, buffer, i, 0);
    EditorScrollFocusedToCursor(ed);
    return;
  }
}

String8 ExplorerEntryUnderCursor(Arena *arena, Buffer *buffer, View *view) {
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload || !view) return String8{nullptr, 0};

  TempArena scratch = ScratchBegin1(arena);
  String8 line = BufferLineText(scratch.arena, buffer, ViewCursorLine(view, buffer));
  ExplorerLine parsed = ExplorerParseLine(line, payload->snapshot.count + 1);

  // A line with no id names something that does not exist yet. Opening it would
  // create a buffer for a path the user has not agreed to create.
  if (parsed.id == 0 || parsed.blank) {
    ScratchEnd(scratch);
    return String8{nullptr, 0};
  }

  String8 name = parsed.name;
  while (name.size > 0 && name.str[name.size - 1] == '/') name = Str8Chop(name, 1);

  String8 path = OsPathJoin(arena, payload->dir, name);
  ScratchEnd(scratch);
  return path;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

namespace {

bool ExplorerOnWrite(Editor *ed, Buffer *buffer, View *view) {
  (void)view;
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload) return false;

  TempArena scratch = ScratchBegin1(payload->pending_arena);

  u64 line_count = BufferLineCount(buffer);
  String8 *lines = PushArray(scratch.arena, String8, Max(line_count, (u64)1));
  for (u64 i = 0; i < line_count; i += 1) {
    lines[i] = BufferLineText(scratch.arena, buffer, i);
  }

  ArenaClear(payload->pending_arena);
  payload->pending = ExplorerDiff(payload->pending_arena, &payload->snapshot, lines, line_count);

  ScratchEnd(scratch);

  if (payload->pending.error.size != 0) {
    EditorSetStatusF(ed, "%.*s", (int)payload->pending.error.size,
                     (char *)payload->pending.error.str);
    payload->pending.count = 0;
    return true;
  }

  if (payload->pending.count == 0) {
    buffer->flags &= ~BufferFlags::Dirty;
    EditorSetStatus(ed, Str8Lit("no changes"));
    return true;
  }

  TempArena summary_scratch = ScratchBegin1(payload->pending_arena);
  String8 summary = ExplorerPlanSummary(summary_scratch.arena, &payload->pending);
  EditorSetStatusF(ed, "%.*s. Apply? [y/N]", (int)summary.size, (char *)summary.str);
  ScratchEnd(summary_scratch);

  EditorAwaitConfirm(ed, CommandId::explorer_apply, buffer->handle);
  return true;
}

}  // namespace

void ExplorerApplyPending(Editor *ed, Buffer *buffer) {
  ExplorerPayload *payload = PayloadFor(buffer);
  if (!payload || payload->pending.count == 0) return;

  TempArena scratch = ScratchBegin1(payload->pending_arena);
  ExplorerApplyResult result = ExplorerApply(scratch.arena, &payload->pending);

  // Point any buffer open on a moved path at its new location before the plan
  // is discarded, so an open file follows its rename instead of silently
  // writing back to a path that no longer exists. Only operations that actually
  // happened count: a failed rename must leave its buffer pointing where the
  // file still is.
  for (u64 i = 0; i < payload->pending.count; i += 1) {
    ExplorerOp *op = &payload->pending.ops[i];
    if (!op->done) continue;

    if (op->kind == ExplorerOpKind::Move) EditorRetargetBufferPaths(ed, op->from, op->to);
    if (op->kind == ExplorerOpKind::Delete || op->kind == ExplorerOpKind::DeleteDir) {
      EditorOrphanBufferPaths(ed, op->from);
    }
  }

  if (result.failed != 0) {
    EditorSetStatusF(ed, "%llu applied, %llu failed (%.*s)", (unsigned long long)result.applied,
                     (unsigned long long)result.failed, (int)result.first_error.size,
                     (char *)result.first_error.str);
  } else {
    EditorSetStatusF(ed, "%llu change%s applied", (unsigned long long)result.applied,
                     result.applied == 1 ? "" : "s");
  }

  ScratchEnd(scratch);

  ArenaClear(payload->pending_arena);
  payload->pending = ExplorerPlan{};

  ExplorerBufferReload(ed, buffer);
}
