#include "buffers/buf_compile.h"

#include "editor/editor.h"
#include "input/keymap.h"
#include "os/os_file.h"
#include "os/os_process.h"

#include <mutex>
#include <new>
#include <thread>

// Compile output as an ordinary buffer. Background threads read the child
// process pipes and queue chunks; EditorTick appends them on the main thread
// so BufferReplace stays single-threaded.
//
// CompilePayload is heap-allocated: it owns a std::mutex and std::thread, which
// must be constructed and destroyed, not arena-zeroed.
//
// Error navigation reparses the buffer on each jump. Compile output is small,
// and re-scanning avoids tracking partial lines across async chunks.

namespace {

constexpr u64 kCompileReadChunk = 4096;
constexpr u64 kNoErrorIndex = ~(u64)0;

struct CompileChunk {
  CompileChunk *next;
  String8 data;
};

struct CompileError {
  String8 path;
  u64 line;         // 1-based
  u64 column;       // 1-based
  u64 buffer_line;  // 0-based line in the [compile] buffer
};

struct CompilePayload {
  Editor *ed = nullptr;
  OsProcess process = {};

  std::mutex mutex;
  Arena *chunk_arena = nullptr;
  CompileChunk *pending_first = nullptr;
  CompileChunk *pending_last = nullptr;

  std::thread stdout_thread;
  std::thread stderr_thread;
  bool stop_requested = false;
  bool stdout_done = false;
  bool stderr_done = false;
  bool wait_done = false;
  bool finished_footer_written = false;
  bool process_started = false;
  i32 exit_code = 0;

  // Index into the last rebuilt error list. kNoErrorIndex means none yet.
  u64 error_index = kNoErrorIndex;
};

CompilePayload *PayloadOf(Buffer *buffer) {
  return buffer ? (CompilePayload *)buffer->user_data : nullptr;
}

Buffer *FindCompileBuffer(Editor *ed) {
  if (!ed) return nullptr;
  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[compile]"));
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer || buffer->kind != BufferKind::Compile) return nullptr;
  return buffer;
}

void WakeEditor(Editor *ed) {
  if (ed && ed->wake) ed->wake(ed->wake_user_data);
}

void EnqueueChunk(CompilePayload *payload, String8 bytes) {
  if (!payload || bytes.size == 0) return;

  std::lock_guard<std::mutex> lock(payload->mutex);
  CompileChunk *chunk = PushStruct(payload->chunk_arena, CompileChunk);
  chunk->data = PushStr8Copy(payload->chunk_arena, bytes);
  chunk->next = nullptr;
  if (payload->pending_last) {
    payload->pending_last->next = chunk;
  } else {
    payload->pending_first = chunk;
  }
  payload->pending_last = chunk;
}

void MarkStreamDone(CompilePayload *payload, bool stdout_stream) {
  {
    std::lock_guard<std::mutex> lock(payload->mutex);
    if (stdout_stream) {
      payload->stdout_done = true;
    } else {
      payload->stderr_done = true;
    }
  }
  WakeEditor(payload->ed);
}

void StdoutMain(CompilePayload *payload) {
  u8 buffer[kCompileReadChunk];
  for (;;) {
    if (payload->stop_requested) break;
    OsProcessRead read = OsProcessReadStdout(&payload->process, buffer, sizeof(buffer));
    if (read.status == OsProcessReadStatus::Data) {
      EnqueueChunk(payload, Str8(buffer, read.size));
      WakeEditor(payload->ed);
      continue;
    }
    break;
  }
  MarkStreamDone(payload, true);
}

void StderrMain(CompilePayload *payload) {
  u8 buffer[kCompileReadChunk];
  for (;;) {
    if (payload->stop_requested) break;
    OsProcessRead read = OsProcessReadStderr(&payload->process, buffer, sizeof(buffer));
    if (read.status == OsProcessReadStatus::Data) {
      EnqueueChunk(payload, Str8(buffer, read.size));
      WakeEditor(payload->ed);
      continue;
    }
    break;
  }
  MarkStreamDone(payload, false);
}

void JoinThreads(CompilePayload *payload) {
  if (payload->stdout_thread.joinable()) payload->stdout_thread.join();
  if (payload->stderr_thread.joinable()) payload->stderr_thread.join();
}

void StopReader(CompilePayload *payload) {
  if (!payload) return;

  payload->stop_requested = true;
  if (payload->process_started) {
    OsProcessTerminate(&payload->process);
    OsProcessCloseStdin(&payload->process);
  }

  JoinThreads(payload);

  if (payload->process_started) {
    (void)OsProcessTryWait(&payload->process, &payload->exit_code);
    if (!OsProcessHasExited(&payload->process)) (void)OsProcessWait(&payload->process);
    OsProcessDestroy(&payload->process);
    payload->process = OsProcess{};
    payload->process_started = false;
  }

  payload->stop_requested = false;
  payload->stdout_done = true;
  payload->stderr_done = true;
  payload->wait_done = true;
}

void DestroyPayload(Buffer *buffer) {
  CompilePayload *payload = PayloadOf(buffer);
  if (!payload) return;
  StopReader(payload);
  if (payload->chunk_arena) ArenaRelease(payload->chunk_arena);
  delete payload;
  buffer->user_data = nullptr;
  buffer->hooks.on_close = nullptr;
  buffer->hooks.on_submit = nullptr;
}

void CompileClose(Editor *ed, Buffer *buffer) {
  (void)ed;
  DestroyPayload(buffer);
}

void AppendReadOnly(Editor *ed, Buffer *buffer, String8 text) {
  if (text.size == 0) return;
  buffer->flags &= ~BufferFlags::ReadOnly;
  u64 at = BufferSize(buffer);
  BufferInsert(ed, buffer, at, text, at, at + text.size);
  buffer->flags |= BufferFlags::ReadOnly;
}

bool DrainPending(Editor *ed, Buffer *buffer, CompilePayload *payload) {
  CompileChunk *first = nullptr;
  bool finished = false;
  i32 exit_code = 0;

  {
    std::lock_guard<std::mutex> lock(payload->mutex);
    first = payload->pending_first;
    payload->pending_first = nullptr;
    payload->pending_last = nullptr;
    finished = payload->stdout_done && payload->stderr_done;
    exit_code = payload->exit_code;
  }

  bool changed = false;
  for (CompileChunk *chunk = first; chunk; chunk = chunk->next) {
    AppendReadOnly(ed, buffer, chunk->data);
    changed = true;
  }

  if (finished && !payload->finished_footer_written) {
    if (payload->process_started && !payload->wait_done) {
      payload->exit_code = OsProcessWait(&payload->process);
      payload->wait_done = true;
      exit_code = payload->exit_code;
    }

    JoinThreads(payload);

    TempArena scratch = ScratchBegin();
    String8 footer =
        PushStr8F(scratch.arena, "\nCompilation finished with exit code %d\n", (int)exit_code);
    AppendReadOnly(ed, buffer, footer);
    ScratchEnd(scratch);
    payload->finished_footer_written = true;
    changed = true;

    if (payload->process_started) {
      OsProcessDestroy(&payload->process);
      payload->process = OsProcess{};
      payload->process_started = false;
    }
  }

  return changed;
}

bool StartShellCommand(CompilePayload *payload, String8 command, String8 cwd) {
  TempArena scratch = ScratchBegin();

  String8 executable = OsShellExecutable(scratch.arena);
#if defined(_WIN32)
  String8 arguments[] = {Str8Lit("/c"), command};
#else
  String8 arguments[] = {Str8Lit("-c"), command};
#endif

  OsProcessCommand proc = {};
  proc.executable = executable;
  proc.arguments = arguments;
  proc.argument_count = ArrayCount(arguments);
  proc.working_directory = cwd;

  bool ok = OsProcessStart(&payload->process, &proc);
  ScratchEnd(scratch);
  if (!ok) return false;

  OsProcessCloseStdin(&payload->process);
  payload->process_started = true;
  return true;
}

bool PathLooksAbsolute(String8 path) {
  if (path.size == 0) return false;
  if (path.str[0] == '/' || path.str[0] == '\\') return true;
#if defined(_WIN32)
  if (path.size >= 3 && CharIsAlpha(path.str[0]) && path.str[1] == ':' &&
      (path.str[2] == '/' || path.str[2] == '\\')) {
    return true;
  }
#endif
  return false;
}

bool PathLooksLikeFile(String8 path) {
  if (path.size == 0) return false;
  for (u64 i = 0; i < path.size; i += 1) {
    u8 c = path.str[i];
    if (c == '/' || c == '\\' || c == '.') return true;
  }
  for (u64 i = 0; i < path.size; i += 1) {
    u8 c = path.str[i];
    if (CharIsAlnum(c) || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool ParseLeadingDigits(String8 s, u64 start, u64 *end, u64 *value) {
  if (start >= s.size || !CharIsDigit(s.str[start])) return false;
  u64 v = 0;
  u64 i = start;
  while (i < s.size && CharIsDigit(s.str[i])) {
    v = v * 10 + (u64)(s.str[i] - '0');
    i += 1;
  }
  *end = i;
  *value = v;
  return true;
}

// GNU/clang diagnostics: path:line:col: message  or  path:line: message.
bool ParseGnuErrorLine(String8 line, String8 *out_path, u64 *out_line, u64 *out_col) {
  while (line.size > 0 && CharIsSpace(line.str[0])) {
    line = Str8Skip(line, 1);
  }
  if (line.size == 0) return false;

  u64 search_from = 0;
  if (line.size >= 2 && CharIsAlpha(line.str[0]) && line.str[1] == ':') {
    search_from = 2;
  }

  for (u64 i = search_from; i < line.size; i += 1) {
    if (line.str[i] != ':') continue;

    u64 after_line = 0;
    u64 line_no = 0;
    if (!ParseLeadingDigits(line, i + 1, &after_line, &line_no)) continue;
    if (line_no == 0) continue;
    if (after_line >= line.size || line.str[after_line] != ':') continue;

    String8 path = Str8Prefix(line, i);
    if (!PathLooksLikeFile(path)) continue;

    u64 after_col = 0;
    u64 col = 0;
    if (ParseLeadingDigits(line, after_line + 1, &after_col, &col) && after_col < line.size &&
        line.str[after_col] == ':') {
      *out_path = path;
      *out_line = line_no;
      *out_col = col == 0 ? 1 : col;
      return true;
    }

    *out_path = path;
    *out_line = line_no;
    *out_col = 1;
    return true;
  }
  return false;
}

struct CompileErrorList {
  CompileError *items;
  u64 count;
};

CompileErrorList RebuildErrors(Arena *arena, Buffer *buffer) {
  CompileErrorList list = {};
  if (!buffer) return list;

  u64 line_count = BufferLineCount(buffer);
  // First pass: count.
  u64 count = 0;
  for (u64 line = 0; line < line_count; line += 1) {
    TempArena scratch = ScratchBegin1(arena);
    String8 text = BufferLineText(scratch.arena, buffer, line);
    String8 path = {};
    u64 row = 0;
    u64 col = 0;
    if (ParseGnuErrorLine(text, &path, &row, &col)) count += 1;
    ScratchEnd(scratch);
  }

  if (count == 0) return list;

  list.items = PushArray(arena, CompileError, count);
  list.count = count;

  u64 at = 0;
  for (u64 line = 0; line < line_count; line += 1) {
    TempArena scratch = ScratchBegin1(arena);
    String8 text = BufferLineText(scratch.arena, buffer, line);
    String8 path = {};
    u64 row = 0;
    u64 col = 0;
    if (ParseGnuErrorLine(text, &path, &row, &col)) {
      list.items[at].path = PushStr8Copy(arena, path);
      list.items[at].line = row;
      list.items[at].column = col;
      list.items[at].buffer_line = line;
      at += 1;
    }
    ScratchEnd(scratch);
  }

  return list;
}

bool JumpToError(Editor *ed, View *origin, const CompileError *err) {
  if (!ed || !err || err->path.size == 0) return false;

  TempArena scratch = ScratchBegin();
  String8 path = err->path;
  if (!PathLooksAbsolute(path)) {
    path = OsPathJoin(scratch.arena, ed->cwd, path);
  }

  BufferHandle handle = EditorOpenFile(ed, path);
  ScratchEnd(scratch);
  if (handle.index == 0) {
    EditorSetStatusF(ed, "Cannot open %.*s", (int)err->path.size, (char *)err->path.str);
    return false;
  }

  if (origin) EditorPushJump(ed, origin);
  EditorShowBuffer(ed, handle);

  View *target = EditorFocusedView(ed);
  Buffer *opened = EditorBufferForView(ed, target);
  if (target && opened) {
    u64 line = err->line > 0 ? err->line - 1 : 0;
    u64 column = err->column > 0 ? err->column - 1 : 0;
    ViewSetCursorLineColumn(target, opened, line, column);
    EditorScrollFocusedToCursor(ed);
  }
  return true;
}

bool NavigateError(Editor *ed, View *origin, bool next) {
  Buffer *buffer = FindCompileBuffer(ed);
  CompilePayload *payload = PayloadOf(buffer);
  if (!payload) {
    EditorSetStatus(ed, Str8Lit("No compile errors"));
    return false;
  }

  TempArena scratch = ScratchBegin();
  CompileErrorList list = RebuildErrors(scratch.arena, buffer);
  if (list.count == 0) {
    EditorSetStatus(ed, Str8Lit("No compile errors"));
    ScratchEnd(scratch);
    return false;
  }

  u64 index = 0;
  if (payload->error_index == kNoErrorIndex || payload->error_index >= list.count) {
    index = next ? 0 : (list.count - 1);
  } else if (next) {
    index = (payload->error_index + 1) % list.count;
  } else {
    index = (payload->error_index + list.count - 1) % list.count;
  }

  payload->error_index = index;
  bool ok = JumpToError(ed, origin, &list.items[index]);
  ScratchEnd(scratch);
  return ok;
}

void CompileSubmit(Editor *ed, Buffer *buffer, View *view, String8 line) {
  (void)line;
  CompilePayload *payload = PayloadOf(buffer);
  if (!payload || !view) return;

  u64 cursor_line = ViewCursorLine(view, buffer);

  TempArena scratch = ScratchBegin();
  CompileErrorList list = RebuildErrors(scratch.arena, buffer);
  for (u64 i = 0; i < list.count; i += 1) {
    if (list.items[i].buffer_line == cursor_line) {
      payload->error_index = i;
      JumpToError(ed, view, &list.items[i]);
      ScratchEnd(scratch);
      return;
    }
  }
  ScratchEnd(scratch);
}

void EnsureCompileKeymap(Editor *ed, Buffer *buffer) {
  if (!ed || !buffer) return;
  if (!buffer->hooks.keymap) {
    Keymap *keymap = KeymapAlloc(ed->arena, ed->normal_map);
    KeymapBind(keymap, "<CR>", CommandId::result_open);
    buffer->hooks.keymap = keymap;
  }
  buffer->hooks.on_submit = CompileSubmit;
}

}  // namespace

String8 CompilePrefillCommand(const Editor *ed) {
  if (ed && ed->last_compile_command.size > 0) return ed->last_compile_command;
  return Str8Lit("make -k");
}

void CompileRememberCommand(Editor *ed, String8 command) {
  if (!ed) return;
  if (!ed->compile_arena) ed->compile_arena = ArenaAlloc();
  ArenaClear(ed->compile_arena);
  ed->last_compile_command = PushStr8Copy(ed->compile_arena, command);
}

BufferHandle CompileBufferRun(Editor *ed, String8 command) {
  if (!ed || Str8SkipChopWhitespace(command).size == 0) return BufferHandleZero();

  command = Str8SkipChopWhitespace(command);
  CompileRememberCommand(ed, command);

  BufferHandle handle = BufferFromName(&ed->buffers, Str8Lit("[compile]"));
  if (handle.index == 0) {
    handle = BufferOpen(&ed->buffers, BufferKind::Compile, Str8Lit("[compile]"));
  }

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) return BufferHandleZero();

  CompilePayload *payload = PayloadOf(buffer);
  if (payload) {
    StopReader(payload);
    if (payload->chunk_arena) ArenaClear(payload->chunk_arena);
  } else {
    payload = new (std::nothrow) CompilePayload();
    if (!payload) return BufferHandleZero();
    payload->chunk_arena = ArenaAlloc(MB(16));
    buffer->user_data = payload;
    buffer->hooks.on_close = CompileClose;
  }

  payload->ed = ed;
  payload->pending_first = nullptr;
  payload->pending_last = nullptr;
  payload->stop_requested = false;
  payload->stdout_done = false;
  payload->stderr_done = false;
  payload->wait_done = false;
  payload->finished_footer_written = false;
  payload->exit_code = 0;
  payload->process = OsProcess{};
  payload->process_started = false;
  payload->error_index = kNoErrorIndex;

  EnsureCompileKeymap(ed, buffer);

  TempArena scratch = ScratchBegin();
  String8 header =
      PushStr8F(scratch.arena, "-*- compile: %.*s -*-\n\n", (int)command.size, (char *)command.str);
  buffer->flags &= ~BufferFlags::ReadOnly;
  BufferSetText(ed, buffer, header);
  buffer->flags |= BufferFlags::ReadOnly;
  ScratchEnd(scratch);

  if (!StartShellCommand(payload, command, ed->cwd)) {
    AppendReadOnly(ed, buffer, Str8Lit("Failed to start process\n"));
    payload->finished_footer_written = true;
    payload->stdout_done = true;
    payload->stderr_done = true;
    payload->wait_done = true;
    return handle;
  }

  try {
    payload->stdout_thread = std::thread(StdoutMain, payload);
    payload->stderr_thread = std::thread(StderrMain, payload);
  } catch (...) {
    OsProcessTerminate(&payload->process);
    OsProcessDestroy(&payload->process);
    payload->process_started = false;
    AppendReadOnly(ed, buffer, Str8Lit("Failed to start reader thread\n"));
    payload->finished_footer_written = true;
    payload->stdout_done = true;
    payload->stderr_done = true;
    payload->wait_done = true;
  }

  return handle;
}

bool CompileBufferTick(Editor *ed) {
  Buffer *buffer = FindCompileBuffer(ed);
  CompilePayload *payload = PayloadOf(buffer);
  if (!payload) return false;
  return DrainPending(ed, buffer, payload);
}

void CompileBufferShutdown(Editor *ed) {
  Buffer *buffer = FindCompileBuffer(ed);
  if (!buffer) return;
  DestroyPayload(buffer);
}

bool CompileNextError(Editor *ed, View *origin) { return NavigateError(ed, origin, true); }

bool CompilePrevError(Editor *ed, View *origin) { return NavigateError(ed, origin, false); }
