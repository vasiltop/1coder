#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"
#include "search/search.h"

// Grep results and the fuzzy file finder, both as ordinary buffers.
//
// Neither introduces a UI concept. A results list is a read-only buffer whose
// keymap claims <CR>; a fuzzy finder is the same thing with an editable query
// on its first line, refiltered from the on_edit hook as you type. Everything
// else -- scrolling, splits, search, vim motions -- is inherited, because these
// are buffers like any other.
//
// The payload for each hangs off `user_data`, which is the whole extension
// mechanism: a kind, a hooks table, and a struct.

namespace {

// ---------------------------------------------------------------------------
// Grep results
// ---------------------------------------------------------------------------

struct GrepPayload {
  GrepResults results;
  String8 root;
  Arena *arena;
};

// <CR> on a result opens the file at that line.
void GrepSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  GrepPayload *payload = (GrepPayload *)buffer->user_data;
  if (!payload) return;

  // The buffer's first two lines are the header, so results start below them.
  u64 cursor_line = ViewCursorLine(view, buffer);
  if (cursor_line < 2) return;

  u64 index = cursor_line - 2;
  if (index >= payload->results.count) return;

  GrepMatch *match = &payload->results.matches[index];

  TempArena scratch = ScratchBegin();
  String8 path = OsPathJoin(scratch.arena, payload->root, match->path);
  BufferHandle handle = EditorOpenFile(ed, path);
  ScratchEnd(scratch);

  if (handle.index == 0) return;
  EditorShowBuffer(ed, handle);

  // Land on the matching line and column rather than the top of the file.
  View *target = EditorFocusedView(ed);
  Buffer *opened = EditorBufferForView(ed, target);
  if (target && opened) {
    ViewSetCursorLineColumn(target, opened, match->line - 1, match->column - 1);
    EditorScrollFocusedToCursor(ed);
  }
}

// ---------------------------------------------------------------------------
// Fuzzy file finder
// ---------------------------------------------------------------------------

struct FinderPayload {
  PathList files;
  String8 root;
  Arena *arena;
  // Guards the rewrite the on_edit hook performs: refiltering edits the buffer,
  // which would otherwise call straight back into on_edit.
  bool updating;
};

// Rewrites everything below the query line to match what has been typed.
void FinderRefilter(Editor *ed, Buffer *buffer) {
  FinderPayload *payload = (FinderPayload *)buffer->user_data;
  if (!payload || payload->updating) return;

  payload->updating = true;

  TempArena scratch = ScratchBegin();
  String8 query = Str8SkipChopWhitespace(BufferLineText(scratch.arena, buffer, 0));

  FuzzyResults matches =
      FuzzyFilter(scratch.arena, payload->files.paths, payload->files.count, query, 200);

  String8List lines = {};
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});  // keeps the query line
  for (u64 i = 0; i < matches.count; i += 1) {
    Str8ListPush(scratch.arena, &lines, matches.items[i].text);
  }

  String8 body = Str8ListJoin(scratch.arena, &lines, Str8Lit("\n"));

  // Replace from the end of the query line onwards, leaving what was typed --
  // and the cursor sitting in it -- untouched.
  u64 query_end = BufferLineEnd(buffer, 0);
  bool read_only = BufferIsReadOnly(buffer);
  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferReplace(ed, buffer, RangeU64{query_end, BufferSize(buffer)}, body, query_end, query_end);
  if (read_only) buffer->flags |= BufferFlags::ReadOnly;

  ScratchEnd(scratch);
  payload->updating = false;
}

void FinderOnEdit(Editor *ed, Buffer *buffer, RangeU64 range, u64 new_len) {
  FinderRefilter(ed, buffer);
}

// <CR> opens the result under the cursor, or the best match when still typing.
void FinderSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  FinderPayload *payload = (FinderPayload *)buffer->user_data;
  if (!payload) return;

  u64 cursor_line = ViewCursorLine(view, buffer);
  // Line 0 is the query, so Enter there means "take the top result".
  u64 target_line = (cursor_line == 0) ? 1 : cursor_line;
  if (target_line >= BufferLineCount(buffer)) return;

  TempArena scratch = ScratchBegin();
  String8 relative = Str8SkipChopWhitespace(BufferLineText(scratch.arena, buffer, target_line));

  if (relative.size > 0) {
    String8 path = OsPathJoin(scratch.arena, payload->root, relative);
    BufferHandle handle = EditorOpenFile(ed, path);
    if (handle.index != 0) EditorShowBuffer(ed, handle);
  }
  ScratchEnd(scratch);
}

}  // namespace

// ---------------------------------------------------------------------------
// Opening them
// ---------------------------------------------------------------------------

BufferHandle GrepBufferOpen(Editor *ed, String8 pattern) {
  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[grep]"));
  if (handle.index == 0) handle = BufferOpen(&ed->buffers, BufferKind::Grep, Str8Lit("[grep]"));

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  GrepPayload *payload = PushStruct(buffer->arena, GrepPayload);
  payload->arena = buffer->arena;
  payload->root = PushStr8Copy(buffer->arena, ed->cwd);
  payload->results = SearchGrep(buffer->arena, payload->root, pattern);

  buffer->user_data = payload;

  TempArena scratch = ScratchBegin();
  String8List lines = {};
  Str8ListPush(scratch.arena, &lines,
               PushStr8F(scratch.arena, "%llu matches for \"%.*s\"%s",
                         (unsigned long long)payload->results.count, (int)pattern.size,
                         (char *)pattern.str, payload->results.truncated ? " (truncated)" : ""));
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});

  for (u64 i = 0; i < payload->results.count; i += 1) {
    GrepMatch *m = &payload->results.matches[i];
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%.*s:%llu: %.*s", (int)m->path.size,
                           (char *)m->path.str, (unsigned long long)m->line, (int)m->text.size,
                           (char *)m->text.str));
  }

  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, Str8ListJoin(scratch.arena, &lines, Str8Lit("\n")));
  buffer->flags |= BufferFlags::ReadOnly;
  ScratchEnd(scratch);

  if (!buffer->hooks.keymap) {
    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "<CR>", CommandId::result_open);
    buffer->hooks.keymap = keymap;
  }
  buffer->hooks.on_submit = GrepSubmit;

  return handle;
}

BufferHandle FinderBufferOpen(Editor *ed) {
  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[files]"));
  if (handle.index == 0) {
    handle = BufferOpen(&ed->buffers, BufferKind::FileList, Str8Lit("[files]"));
  }

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  FinderPayload *payload = PushStruct(buffer->arena, FinderPayload);
  payload->arena = buffer->arena;
  payload->root = PushStr8Copy(buffer->arena, ed->cwd);
  payload->files = SearchWalkFiles(buffer->arena, payload->root);
  payload->updating = false;

  buffer->user_data = payload;
  buffer->hooks.on_edit = FinderOnEdit;
  buffer->hooks.on_submit = FinderSubmit;

  if (!buffer->hooks.keymap) {
    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "<CR>", CommandId::result_open);
    buffer->hooks.keymap = keymap;
  }

  // Start empty, which lists everything, then let the hook do the rest.
  BufferSetText(ed, buffer, String8{nullptr, 0});
  FinderRefilter(ed, buffer);

  return handle;
}
