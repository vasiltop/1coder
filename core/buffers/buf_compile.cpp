#include "buffers/buf_compile.h"

#include "editor/editor.h"
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

namespace {

constexpr u64 kCompileReadChunk = 4096;

struct CompileChunk {
  CompileChunk *next;
  String8 data;
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
