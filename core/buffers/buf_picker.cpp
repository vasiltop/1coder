#include "editor/command.h"
#include "editor/editor.h"
#include "editor/filetype.h"
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

  // Which buffer line the first result sits on: below a header for a one-shot
  // search, below the query for a live one.
  u64 first_result_line;

  // Live search only. The corpus is read once and rescanned on every keystroke;
  // re-reading the tree per keystroke would not be usable.
  bool live;
  bool updating;
  SearchCorpus corpus;
  Arena *corpus_arena;   // holds the corpus for the buffer's lifetime
  Arena *results_arena;  // cleared and refilled on every query
};

// Releases what the payload owns. Buffers can be closed, and a corpus is large
// enough that leaking one matters.
void GrepClose(Editor *ed, Buffer *buffer) {
  GrepPayload *payload = (GrepPayload *)buffer->user_data;
  if (!payload) return;

  if (payload->corpus_arena) ArenaRelease(payload->corpus_arena);
  if (payload->results_arena) ArenaRelease(payload->results_arena);
  payload->corpus_arena = nullptr;
  payload->results_arena = nullptr;
}

// <CR> on a result opens the file at that line.
void GrepSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  GrepPayload *payload = (GrepPayload *)buffer->user_data;
  if (!payload) return;

  // Enter while still typing takes the first match, so a search can be run and
  // followed without ever leaving insert mode. The same applies above a
  // one-shot search's header, where there is nothing else Enter could mean.
  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 index = (cursor_line < payload->first_result_line)
                  ? 0
                  : cursor_line - payload->first_result_line;

  if (index >= payload->results.count) return;

  GrepMatch *match = &payload->results.matches[index];

  TempArena scratch = ScratchBegin();
  String8 path = OsPathJoin(scratch.arena, payload->root, match->path);
  BufferHandle handle = EditorOpenFile(ed, path);
  ScratchEnd(scratch);

  if (handle.index == 0) return;
  EditorPushJump(ed, view);
  EditorShowBuffer(ed, handle);

  // Land on the matching line and column rather than the top of the file.
  View *target = EditorFocusedView(ed);
  Buffer *opened = EditorBufferForView(ed, target);
  if (target && opened) {
    ViewSetCursorLineColumn(target, opened, match->line - 1, match->column - 1);
    EditorScrollFocusedToCursor(ed);
  }
}

// Reruns the search for whatever is on the query line and rewrites the results
// below it. Same shape as the file finder: the buffer's first line is the
// input, everything under it is output.
void LiveGrepRefilter(Editor *ed, Buffer *buffer) {
  GrepPayload *payload = (GrepPayload *)buffer->user_data;
  if (!payload || !payload->live || payload->updating) return;

  payload->updating = true;

  TempArena scratch = ScratchBegin();
  String8 query = Str8SkipChopWhitespace(BufferLineText(scratch.arena, buffer, 0));

  // The previous query's results are thrown away wholesale rather than
  // accumulating one set per keystroke.
  ArenaClear(payload->results_arena);
  payload->results = SearchGrepCorpus(payload->results_arena, &payload->corpus, query, 200);

  String8List lines = {};
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});  // keeps the query line

  for (u64 i = 0; i < payload->results.count; i += 1) {
    GrepMatch *m = &payload->results.matches[i];
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%.*s:%llu: %.*s", (int)m->path.size,
                           (char *)m->path.str, (unsigned long long)m->line, (int)m->text.size,
                           (char *)m->text.str));
  }

  u64 query_end = BufferLineEnd(buffer, 0);
  String8 body = Str8ListJoin(scratch.arena, &lines, Str8Lit("\n"));
  // Refilter rewrites the results region; clear guards that would block it.
  bool read_only = BufferIsReadOnly(buffer);
  bool query_only = BufferIsQueryOnly(buffer);
  buffer->flags &= ~(BufferFlags::ReadOnly | BufferFlags::QueryOnly);
  BufferReplace(ed, buffer, RangeU64{query_end, BufferSize(buffer)}, body, query_end, query_end);
  if (read_only) buffer->flags |= BufferFlags::ReadOnly;
  if (query_only) buffer->flags |= BufferFlags::QueryOnly;

  ScratchEnd(scratch);
  payload->updating = false;
}

void LiveGrepOnEdit(Editor *ed, Buffer *buffer, RangeU64 range, u64 new_len) {
  LiveGrepRefilter(ed, buffer);
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
  bool query_only = BufferIsQueryOnly(buffer);
  buffer->flags &= ~(BufferFlags::ReadOnly | BufferFlags::QueryOnly);
  BufferReplace(ed, buffer, RangeU64{query_end, BufferSize(buffer)}, body, query_end, query_end);
  if (read_only) buffer->flags |= BufferFlags::ReadOnly;
  if (query_only) buffer->flags |= BufferFlags::QueryOnly;

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
    BufferHandle handle = FiletypeOpen(ed, path);
    if (handle.index != 0) {
      EditorPushJump(ed, view);
      EditorShowBuffer(ed, handle);
    }
  }
  ScratchEnd(scratch);
}

// ---------------------------------------------------------------------------
// Buffer picker
// ---------------------------------------------------------------------------

// Fuzzy-picks among open buffers. Filtered handles sit parallel to the result
// lines so <CR> can switch without reopening a file by path.
struct BuffersPayload {
  BufferHandle handles[200];
  u64 count;
  bool updating;
};

String8 BufferPickerDisplay(Arena *arena, Buffer *b) {
  if (b->path.size == 0) return PushStr8Copy(arena, b->name);
  return PushStr8F(arena, "%.*s  %.*s", (int)b->name.size, (char *)b->name.str, (int)b->path.size,
                   (char *)b->path.str);
}

void BuffersRefilter(Editor *ed, Buffer *buffer) {
  BuffersPayload *payload = (BuffersPayload *)buffer->user_data;
  if (!payload || payload->updating) return;

  payload->updating = true;

  TempArena scratch = ScratchBegin();
  String8 query = Str8SkipChopWhitespace(BufferLineText(scratch.arena, buffer, 0));

  u64 cand_count = 0;
  for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0; h = BufferNext(&ed->buffers, h)) {
    Buffer *b = BufferFromHandle(&ed->buffers, h);
    if (!b || Str8Match(b->name, Str8Lit("[buffers]"))) continue;
    cand_count += 1;
  }

  String8 *candidates = PushArray(scratch.arena, String8, Max(cand_count, (u64)1));
  BufferHandle *cand_handles = PushArray(scratch.arena, BufferHandle, Max(cand_count, (u64)1));
  u64 n = 0;
  for (BufferHandle h = BufferFirst(&ed->buffers); h.index != 0; h = BufferNext(&ed->buffers, h)) {
    Buffer *b = BufferFromHandle(&ed->buffers, h);
    if (!b || Str8Match(b->name, Str8Lit("[buffers]"))) continue;
    candidates[n] = BufferPickerDisplay(scratch.arena, b);
    cand_handles[n] = h;
    n += 1;
  }

  FuzzyResults matches = FuzzyFilter(scratch.arena, candidates, n, query, ArrayCount(payload->handles));

  payload->count = 0;
  String8List lines = {};
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});  // keeps the query line
  for (u64 i = 0; i < matches.count; i += 1) {
    // FuzzyFilter keeps the candidate String8 pointer, so identity finds the handle.
    BufferHandle handle = {};
    for (u64 j = 0; j < n; j += 1) {
      if (matches.items[i].text.str == candidates[j].str) {
        handle = cand_handles[j];
        break;
      }
    }
    payload->handles[payload->count++] = handle;
    Str8ListPush(scratch.arena, &lines, matches.items[i].text);
  }

  String8 body = Str8ListJoin(scratch.arena, &lines, Str8Lit("\n"));
  u64 query_end = BufferLineEnd(buffer, 0);
  bool read_only = BufferIsReadOnly(buffer);
  bool query_only = BufferIsQueryOnly(buffer);
  buffer->flags &= ~(BufferFlags::ReadOnly | BufferFlags::QueryOnly);
  BufferReplace(ed, buffer, RangeU64{query_end, BufferSize(buffer)}, body, query_end, query_end);
  if (read_only) buffer->flags |= BufferFlags::ReadOnly;
  if (query_only) buffer->flags |= BufferFlags::QueryOnly;

  ScratchEnd(scratch);
  payload->updating = false;
}

void BuffersOnEdit(Editor *ed, Buffer *buffer, RangeU64 range, u64 new_len) {
  BuffersRefilter(ed, buffer);
}

// <CR> switches to the buffer under the cursor, or the top match when typing.
void BuffersSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  BuffersPayload *payload = (BuffersPayload *)buffer->user_data;
  if (!payload) return;

  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 index = (cursor_line == 0) ? 0 : cursor_line - 1;
  if (index >= payload->count) return;

  BufferHandle handle = payload->handles[index];
  if (handle.index == 0 || !BufferFromHandle(&ed->buffers, handle)) return;

  EditorPushJump(ed, view);
  EditorShowBuffer(ed, handle);
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
  payload->root = PushStr8Copy(buffer->arena, ed->cwd);
  payload->first_result_line = 2;  // below the header and its blank line
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

BufferHandle LiveGrepBufferOpen(Editor *ed) {
  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[live-grep]"));
  bool fresh = (handle.index == 0);
  if (fresh) handle = BufferOpen(&ed->buffers, BufferKind::Grep, Str8Lit("[live-grep]"));

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  // Reopening reuses the corpus already in memory, so only the first open pays
  // for reading the tree.
  GrepPayload *payload = (GrepPayload *)buffer->user_data;
  if (!payload) {
    payload = PushStruct(buffer->arena, GrepPayload);
    payload->live = true;
    payload->first_result_line = 1;
    payload->root = PushStr8Copy(buffer->arena, ed->cwd);
    payload->corpus_arena = ArenaAlloc(MB(256));
    payload->results_arena = ArenaAlloc(MB(64));
    payload->corpus = SearchLoadCorpus(payload->corpus_arena, payload->root);

    buffer->user_data = payload;
    buffer->hooks.on_edit = LiveGrepOnEdit;
    buffer->hooks.on_submit = GrepSubmit;
    buffer->hooks.on_close = GrepClose;

    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "<CR>", CommandId::result_open);
    buffer->hooks.keymap = keymap;
  }

  buffer->flags |= BufferFlags::QueryOnly;
  BufferSetText(ed, buffer, String8{nullptr, 0});
  LiveGrepRefilter(ed, buffer);

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
  buffer->flags |= BufferFlags::QueryOnly;

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

BufferHandle BuffersBufferOpen(Editor *ed) {
  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[buffers]"));
  if (handle.index == 0) {
    handle = BufferOpen(&ed->buffers, BufferKind::FileList, Str8Lit("[buffers]"));
  }

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  BuffersPayload *payload = PushStruct(buffer->arena, BuffersPayload);
  payload->count = 0;
  payload->updating = false;

  buffer->user_data = payload;
  buffer->hooks.on_edit = BuffersOnEdit;
  buffer->hooks.on_submit = BuffersSubmit;
  buffer->flags |= BufferFlags::QueryOnly;

  if (!buffer->hooks.keymap) {
    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "<CR>", CommandId::result_open);
    buffer->hooks.keymap = keymap;
  }

  BufferSetText(ed, buffer, String8{nullptr, 0});
  BuffersRefilter(ed, buffer);

  return handle;
}
