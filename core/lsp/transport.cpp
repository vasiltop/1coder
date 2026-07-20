#include "lsp/transport.h"

#include "lsp/framing.h"
#include "os/os_process.h"

#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <chrono>

namespace {

inline constexpr u64 kLspTransportQueueCap = MB(32);
inline constexpr u64 kLspTransportStderrCap = KB(64);
inline constexpr char kLspTransportStderrTruncated[] = "...[stderr truncated]...\n";
inline constexpr u64 kLspTransportReadChunk = KB(16);
inline constexpr u32 kLspTransportStopGraceMs = 250;

struct QueuedMessage {
  std::string text;
  bool stderr_message;
};

struct LspTransportImpl {
  Arena *arena;
  OsProcess process{};
  LspFrameDecoder decoder;
  LspServerCommand command;
  LspWakeProc wake;
  void *wake_user_data;
  std::mutex mutex;
  std::mutex write_mutex;
  std::thread stdout_thread;
  std::thread stderr_thread;
  std::deque<QueuedMessage> queue;
  std::string failure_reason;
  std::string stderr_summary;
  u64 queued_bytes;
  bool stop_requested;

  LspTransportImpl() : arena(ArenaAlloc(MB(4))), queued_bytes(0), stop_requested(false) {}
  ~LspTransportImpl() {
    if (arena != nullptr) ArenaRelease(arena);
  }
};

LspTransportImpl *GetImpl(const LspTransport *transport) {
  return transport ? (LspTransportImpl *)transport->impl : nullptr;
}

String8 CopyString8(Arena *arena, String8 value) { return PushStr8Copy(arena, value); }

LspServerCommand CopyCommand(Arena *arena, const LspServerCommand *command) {
  LspServerCommand result = {};
  result.language = command->language;
  result.language_id = CopyString8(arena, command->language_id);
  result.executable = CopyString8(arena, command->executable);
  result.argument_count = command->argument_count;
  result.arguments = command->argument_count ? PushArray(arena, String8, command->argument_count) : nullptr;
  for (u64 i = 0; i < command->argument_count; i += 1) {
    result.arguments[i] = CopyString8(arena, command->arguments[i]);
  }
  result.root = CopyString8(arena, command->root);
  return result;
}

void Wake(const LspTransportImpl *impl) {
  if (impl != nullptr && impl->wake != nullptr) impl->wake(impl->wake_user_data);
}

void AppendStderrSummaryLocked(LspTransportImpl *impl, String8 text) {
  if (text.size == 0) return;

  impl->stderr_summary.append((const char *)text.str, (size_t)text.size);
  if (impl->stderr_summary.size() <= kLspTransportStderrCap) return;

  std::string marker(kLspTransportStderrTruncated);
  size_t keep = marker.size() >= kLspTransportStderrCap ? 0
                                                        : (size_t)(kLspTransportStderrCap - marker.size());
  if (keep > impl->stderr_summary.size()) keep = impl->stderr_summary.size();
  std::string suffix = impl->stderr_summary.substr(impl->stderr_summary.size() - keep);
  impl->stderr_summary = marker + suffix;
}

bool SetFailureLocked(LspTransportImpl *impl, String8 reason) {
  if (impl->failure_reason.empty()) {
    impl->failure_reason.assign((const char *)reason.str, (size_t)reason.size);
    return true;
  }
  return false;
}

bool SetFailure(LspTransportImpl *impl, String8 reason) {
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->stop_requested) wake = SetFailureLocked(impl, reason);
  }
  if (wake) Wake(impl);
  return false;
}

bool EnqueueMessage(LspTransportImpl *impl, String8 text, bool stderr_message) {
  bool wake = false;
  bool ok = true;
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (stderr_message) AppendStderrSummaryLocked(impl, text);
    if (impl->stop_requested) return false;

    if (text.size > kLspTransportQueueCap - impl->queued_bytes) {
      wake = SetFailureLocked(impl, Str8Lit("transport queue overflow"));
      ok = false;
    } else {
      QueuedMessage queued = {};
      queued.text.assign((const char *)text.str, (size_t)text.size);
      queued.stderr_message = stderr_message;
      impl->queued_bytes += text.size;
      impl->queue.push_back(std::move(queued));
      wake = true;
    }
  }
  if (wake) Wake(impl);
  return ok;
}

bool TransportIsFailed(LspTransportImpl *impl) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return !impl->failure_reason.empty();
}

void PumpDecodedFrames(LspTransportImpl *impl) {
  while (LspFrameDecoderQueuedCount(&impl->decoder) > 0) {
    TempArena scratch = ScratchBegin();
    String8 json = LspFrameDecoderPop(&impl->decoder, scratch.arena);
    bool ok = EnqueueMessage(impl, json, false);
    ScratchEnd(scratch);
    if (!ok) return;
  }
}

void StdoutThreadMain(LspTransportImpl *impl) {
  u8 buffer[kLspTransportReadChunk];
  for (;;) {
    OsProcessRead read = OsProcessReadStdout(&impl->process, buffer, sizeof(buffer));
    if (read.status == OsProcessReadStatus::Data) {
      if (!LspFrameDecoderFeed(&impl->decoder, Str8(buffer, read.size))) {
        SetFailure(impl, LspFrameDecoderError(&impl->decoder));
        return;
      }
      PumpDecodedFrames(impl);
      if (TransportIsFailed(impl)) return;
      continue;
    }

    if (read.status == OsProcessReadStatus::Error) {
      SetFailure(impl, Str8Lit("language server stdout read failed"));
      return;
    }

    if (!LspFrameDecoderFinish(&impl->decoder)) {
      SetFailure(impl, LspFrameDecoderError(&impl->decoder));
      return;
    }

    bool stopping = false;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      stopping = impl->stop_requested;
    }
    if (!stopping) SetFailure(impl, Str8Lit("language server stdout closed unexpectedly"));
    return;
  }
}

void StderrThreadMain(LspTransportImpl *impl) {
  u8 buffer[kLspTransportReadChunk];
  for (;;) {
    OsProcessRead read = OsProcessReadStderr(&impl->process, buffer, sizeof(buffer));
    if (read.status == OsProcessReadStatus::Data) {
      if (!EnqueueMessage(impl, Str8(buffer, read.size), true)) return;
      continue;
    }

    if (read.status == OsProcessReadStatus::Error) {
      SetFailure(impl, Str8Lit("language server stderr read failed"));
    }
    return;
  }
}

}  // namespace

bool LspTransportStart(LspTransport *transport, const LspTransportConfig *config) {
  if (transport == nullptr || transport->impl != nullptr || config == nullptr ||
      config->command.executable.size == 0) {
    return false;
  }

  LspTransportImpl *impl = new (std::nothrow) LspTransportImpl();
  if (impl == nullptr || impl->arena == nullptr) {
    delete impl;
    return false;
  }

  impl->command = CopyCommand(impl->arena, &config->command);
  impl->wake = config->wake;
  impl->wake_user_data = config->wake_user_data;
  LspFrameDecoderInit(&impl->decoder);

  OsProcessCommand command = {};
  command.executable = impl->command.executable;
  command.arguments = impl->command.arguments;
  command.argument_count = impl->command.argument_count;
  command.working_directory = impl->command.root;
  if (!OsProcessStart(&impl->process, &command)) {
    LspFrameDecoderDestroy(&impl->decoder);
    delete impl;
    return false;
  }

  try {
    impl->stdout_thread = std::thread(StdoutThreadMain, impl);
    impl->stderr_thread = std::thread(StderrThreadMain, impl);
  } catch (...) {
    impl->stop_requested = true;
    OsProcessCloseStdin(&impl->process);
    OsProcessTerminate(&impl->process);
    if (impl->stdout_thread.joinable()) impl->stdout_thread.join();
    if (impl->stderr_thread.joinable()) impl->stderr_thread.join();
    OsProcessDestroy(&impl->process);
    LspFrameDecoderDestroy(&impl->decoder);
    delete impl;
    return false;
  }

  transport->impl = impl;
  return true;
}

bool LspTransportSend(LspTransport *transport, String8 json) {
  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr) return false;

  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->stop_requested || !impl->failure_reason.empty()) return false;
  }

  TempArena scratch = ScratchBegin();
  String8 framed = LspFrameEncode(scratch.arena, json);
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(impl->write_mutex);
    ok = OsProcessWrite(&impl->process, framed);
  }
  ScratchEnd(scratch);
  if (!ok) return SetFailure(impl, Str8Lit("language server stdin write failed"));
  return true;
}

bool LspTransportPop(LspTransport *transport, Arena *arena, LspInboundMessage *message) {
  if (message == nullptr) return false;
  *message = {};

  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr || arena == nullptr) return false;

  std::lock_guard<std::mutex> lock(impl->mutex);
  if (impl->queue.empty()) return false;

  QueuedMessage queued = std::move(impl->queue.front());
  impl->queue.pop_front();
  impl->queued_bytes -= queued.text.size();
  message->json = PushStr8Copy(arena, Str8C(queued.text.c_str()));
  message->stderr_message = queued.stderr_message;
  return true;
}

bool LspTransportFailed(const LspTransport *transport) {
  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl->mutex);
  return !impl->failure_reason.empty();
}

String8 LspTransportFailureReason(const LspTransport *transport) {
  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr) return {};
  static thread_local std::string snapshot;
  std::lock_guard<std::mutex> lock(impl->mutex);
  snapshot = impl->failure_reason;
  return Str8C(snapshot.c_str());
}

String8 LspTransportStderrSummary(const LspTransport *transport) {
  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr) return {};
  static thread_local std::string snapshot;
  std::lock_guard<std::mutex> lock(impl->mutex);
  snapshot = impl->stderr_summary;
  return Str8C(snapshot.c_str());
}

String8 StopTransportImpl(LspTransport *transport, Arena *snapshot_arena) {
  LspTransportImpl *impl = GetImpl(transport);
  if (impl == nullptr) {
    if (transport) *transport = {};
    return {};
  }

  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->stop_requested = true;
  }

  OsProcessCloseStdin(&impl->process);
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kLspTransportStopGraceMs);
  while (!OsProcessHasExited(&impl->process) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  OsProcessTerminate(&impl->process);
  if (impl->stdout_thread.joinable()) impl->stdout_thread.join();
  if (impl->stderr_thread.joinable()) impl->stderr_thread.join();
  String8 summary = {};
  if (snapshot_arena != nullptr && !impl->stderr_summary.empty()) {
    summary = PushStr8Copy(snapshot_arena, Str8C(impl->stderr_summary.c_str()));
  }
  OsProcessDestroy(&impl->process);
  LspFrameDecoderDestroy(&impl->decoder);
  delete impl;
  transport->impl = nullptr;
  return summary;
}

String8 LspTransportStopAndCaptureStderr(LspTransport *transport, Arena *arena) {
  return StopTransportImpl(transport, arena);
}

void LspTransportStop(LspTransport *transport) {
  (void)StopTransportImpl(transport, nullptr);
}
