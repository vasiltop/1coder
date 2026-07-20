#include "lsp/client.h"

#include "lsp/json.h"

#include <chrono>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

inline constexpr u32 kLspClientShutdownTimeoutMs = 150;
inline constexpr u32 kLspClientStopPollMs = 5;
inline constexpr size_t kLspClientIgnoredCancelledCap = 256;

enum class PendingRequestKind : u8 { User, Initialize, Shutdown };

struct PendingRequest {
  u64 id;
  std::string method;
  LspClientRequestContext context;
  LspClientResponseProc response_proc;
  void *response_user_data;
  PendingRequestKind kind;
  bool cancellation_requested;
};

struct LspClientImpl {
  LspClient *owner;
  Arena *arena;
  u64 runtime_pos;
  LspClientConfig config;
  LspTransport transport;
  LspClientState state;
  LspServerCapabilities capabilities;
  LspPositionEncoding position_encoding;
  String8 failure_reason;
  String8 stderr_summary;
  std::string stderr_summary_storage;
  String8 root_uri;
  String8 workspace_name;
  u64 next_request_id;
  u32 tick_depth;
  u32 callback_depth;
  bool restart_consumed;
  bool stop_requested;
  bool destroy_deferred;
  std::vector<PendingRequest> pending;
  std::vector<PendingRequest> cancelled;
  std::vector<u64> ignored_cancelled_ids;

  LspClientImpl()
      : owner(nullptr), arena(ArenaAlloc(MB(8))), runtime_pos(0), state(LspClientState::Stopped),
        position_encoding(LspPositionEncoding::Utf16), next_request_id(0), tick_depth(0),
        callback_depth(0), restart_consumed(false), stop_requested(false), destroy_deferred(false) {}
  ~LspClientImpl() {
    if (arena != nullptr) ArenaRelease(arena);
  }
};

LspClientImpl *GetImpl(const LspClient *client) {
  return client ? (LspClientImpl *)client->impl : nullptr;
}

String8 CopyString8(Arena *arena, String8 value) { return PushStr8Copy(arena, value); }
void SetCachedStderrSummary(LspClientImpl *impl, String8 summary);
void CacheStderrSummary(LspClientImpl *impl);

bool ClientStillOwnsImpl(const LspClientImpl *impl) {
  return impl != nullptr && impl->owner != nullptr && impl->owner->impl == impl;
}

bool HasDeferredLifecycle(const LspClientImpl *impl) {
  return impl != nullptr && (impl->stop_requested || impl->destroy_deferred);
}

struct ScopedTick {
  LspClientImpl *impl;

  explicit ScopedTick(LspClientImpl *impl_) : impl(impl_) {
    if (impl != nullptr) impl->tick_depth += 1;
  }

  ~ScopedTick() {
    if (impl == nullptr) return;
    impl->tick_depth -= 1;
  }
};

struct ScopedCallback {
  LspClientImpl *impl;

  explicit ScopedCallback(LspClientImpl *impl_) : impl(impl_) {
    if (impl != nullptr) impl->callback_depth += 1;
  }

  ~ScopedCallback() {
    if (impl == nullptr) return;
    impl->callback_depth -= 1;
  }
};

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

bool ConsumeId(std::vector<u64> *values, u64 id) {
  if (values == nullptr) return false;
  for (size_t i = 0; i < values->size(); i += 1) {
    if ((*values)[i] == id) {
      values->erase(values->begin() + (long long)i);
      return true;
    }
  }
  return false;
}

String8 RootName(String8 root) {
  String8 base = Str8PathBase(root);
  if (base.size > 0) return base;
  return root;
}

String8 EscapeJsonString(Arena *arena, String8 value) {
  String8List parts = {};
  u64 start = 0;
  for (u64 i = 0; i < value.size; i += 1) {
    String8 replacement = {};
    switch (value.str[i]) {
      case '"': replacement = Str8Lit("\\\""); break;
      case '\\': replacement = Str8Lit("\\\\"); break;
      case '\b': replacement = Str8Lit("\\b"); break;
      case '\f': replacement = Str8Lit("\\f"); break;
      case '\n': replacement = Str8Lit("\\n"); break;
      case '\r': replacement = Str8Lit("\\r"); break;
      case '\t': replacement = Str8Lit("\\t"); break;
      default: {
        if (value.str[i] < 0x20) {
          if (i > start) Str8ListPush(arena, &parts, Str8(value.str + start, i - start));
          Str8ListPush(arena, &parts, PushStr8F(arena, "\\u%04X", value.str[i]));
          start = i + 1;
        }
        continue;
      } break;
    }

    if (i > start) Str8ListPush(arena, &parts, Str8(value.str + start, i - start));
    Str8ListPush(arena, &parts, replacement);
    start = i + 1;
  }
  if (start < value.size) Str8ListPush(arena, &parts, Str8(value.str + start, value.size - start));
  if (parts.node_count == 0) return PushStr8Copy(arena, value);
  return Str8ListJoin(arena, &parts, String8{});
}

bool ValidateJsonValue(Arena *arena, String8 text) {
  JsonParseResult parsed = JsonParse(arena, text);
  return parsed.root != nullptr;
}

String8 BuildEnvelopeFromRaw(Arena *arena, bool has_id, u64 id, String8 method, String8 params_json) {
  TempArena scratch = ScratchBegin1(arena);
  bool ok = params_json.size == 0 || ValidateJsonValue(scratch.arena, params_json);
  ScratchEnd(scratch);
  if (!ok) return {};

  String8 escaped = EscapeJsonString(arena, method);
  if (has_id) {
    if (params_json.size > 0) {
      return PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%.*s\",\"params\":%.*s}",
                       (unsigned long long)id, (int)escaped.size, (char *)escaped.str,
                       (int)params_json.size, (char *)params_json.str);
    }
    return PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%.*s\"}",
                     (unsigned long long)id, (int)escaped.size, (char *)escaped.str);
  }

  if (params_json.size > 0) {
    return PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"method\":\"%.*s\",\"params\":%.*s}",
                     (int)escaped.size, (char *)escaped.str, (int)params_json.size,
                     (char *)params_json.str);
  }
  return PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"method\":\"%.*s\"}", (int)escaped.size,
                   (char *)escaped.str);
}

String8 BuildEnvelopeFromWriter(Arena *arena, bool has_id, u64 id, String8 method,
                                LspClientWriteJsonProc params_writer, void *params_user_data) {
  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);

  bool ok = JsonWriteObjectBegin(&writer) && JsonWriteObjectKey(&writer, Str8Lit("jsonrpc")) &&
            JsonWriteString(&writer, Str8Lit("2.0"));
  if (ok && has_id) ok = JsonWriteObjectKey(&writer, Str8Lit("id")) && JsonWriteU64(&writer, id);
  if (ok) ok = JsonWriteObjectKey(&writer, Str8Lit("method")) && JsonWriteString(&writer, method);
  if (ok && params_writer != nullptr) {
    ok = JsonWriteObjectKey(&writer, Str8Lit("params")) && params_writer(&writer, params_user_data);
  }
  if (ok) ok = JsonWriteObjectEnd(&writer);

  String8 result = ok ? JsonWriterFinish(&writer) : String8{};
  JsonWriterDestroy(&writer);
  return result;
}

bool TransportStart(LspClientImpl *impl, const LspTransportConfig *config) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->start(impl->config.transport_user_data, config);
  }
  return LspTransportStart(&impl->transport, config);
}

bool TransportSend(LspClientImpl *impl, String8 json) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->send(impl->config.transport_user_data, json);
  }
  return LspTransportSend(&impl->transport, json);
}

bool TransportPop(LspClientImpl *impl, Arena *arena, LspInboundMessage *message) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->pop(impl->config.transport_user_data, arena, message);
  }
  return LspTransportPop(&impl->transport, arena, message);
}

void TransportStop(LspClientImpl *impl) {
  if (impl->config.transport_hooks != nullptr) {
    impl->config.transport_hooks->stop(impl->config.transport_user_data);
    CacheStderrSummary(impl);
    return;
  }
  SetCachedStderrSummary(impl, LspTransportStopAndCaptureStderr(&impl->transport, impl->arena));
}

bool TransportFailed(LspClientImpl *impl) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->failed(impl->config.transport_user_data);
  }
  return LspTransportFailed(&impl->transport);
}

String8 TransportFailureReason(LspClientImpl *impl) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->failure_reason(impl->config.transport_user_data);
  }
  return LspTransportFailureReason(&impl->transport);
}

String8 TransportStderrSummary(LspClientImpl *impl) {
  if (impl->config.transport_hooks != nullptr) {
    return impl->config.transport_hooks->stderr_summary(impl->config.transport_user_data);
  }
  return LspTransportStderrSummary(&impl->transport);
}

void SetCachedStderrSummary(LspClientImpl *impl, String8 summary) {
  impl->stderr_summary_storage = std::string((const char *)summary.str, (size_t)summary.size);
  impl->stderr_summary = Str8C(impl->stderr_summary_storage.c_str());
}

void CacheStderrSummary(LspClientImpl *impl) {
  SetCachedStderrSummary(impl, TransportStderrSummary(impl));
}

void ClearRuntimeState(LspClientImpl *impl) {
  impl->pending.clear();
  impl->cancelled.clear();
  impl->ignored_cancelled_ids.clear();
  impl->capabilities = {};
  impl->position_encoding = LspPositionEncoding::Utf16;
  impl->failure_reason = {};
  impl->stop_requested = false;
  ArenaPopTo(impl->arena, impl->runtime_pos);
}

u64 PeekNextRequestId(const LspClientImpl *impl) {
  u64 id = impl->next_request_id;
  do {
    id += 1;
  } while (id == 0);
  return id;
}

void QueueCancelledCompletion(LspClientImpl *impl, PendingRequest request) {
  impl->ignored_cancelled_ids.push_back(request.id);
  if (impl->ignored_cancelled_ids.size() > kLspClientIgnoredCancelledCap) {
    impl->ignored_cancelled_ids.erase(impl->ignored_cancelled_ids.begin());
  }
  impl->cancelled.push_back(std::move(request));
}

void PromoteRequestedCancellations(LspClientImpl *impl) {
  std::vector<PendingRequest> survivors;
  survivors.reserve(impl->pending.size());
  for (PendingRequest &request : impl->pending) {
    if (request.cancellation_requested) {
      QueueCancelledCompletion(impl, std::move(request));
    } else {
      survivors.push_back(std::move(request));
    }
  }
  impl->pending = std::move(survivors);
}

void DispatchCancelled(LspClientImpl *impl) {
  std::vector<PendingRequest> cancelled = std::move(impl->cancelled);
  impl->cancelled.clear();
  for (size_t i = 0; i < cancelled.size(); i += 1) {
    const PendingRequest &request = cancelled[i];
    if (request.kind != PendingRequestKind::User || request.response_proc == nullptr) continue;
    LspClientResponse response = {};
    response.id = request.id;
    response.method = Str8C(request.method.c_str());
    response.context = request.context;
    response.cancelled = true;
    response.error_code = kLspClientErrorCancelled;
    ScopedCallback callback_scope(impl);
    request.response_proc(request.response_user_data, &response);
    if (HasDeferredLifecycle(impl) || !ClientStillOwnsImpl(impl)) {
      for (size_t tail = i + 1; tail < cancelled.size(); tail += 1) {
        impl->cancelled.push_back(std::move(cancelled[tail]));
      }
      break;
    }
  }
}

void FailPendingRequests(LspClientImpl *impl, String8 message, i64 error_code) {
  std::vector<PendingRequest> pending = std::move(impl->pending);
  impl->pending.clear();
  for (size_t i = 0; i < pending.size(); i += 1) {
    const PendingRequest &request = pending[i];
    if (request.kind != PendingRequestKind::User || request.response_proc == nullptr) continue;
    LspClientResponse response = {};
    response.id = request.id;
    response.method = Str8C(request.method.c_str());
    response.context = request.context;
    response.has_error = true;
    response.error_code = error_code;
    response.error_message = message;
    ScopedCallback callback_scope(impl);
    request.response_proc(request.response_user_data, &response);
    if (HasDeferredLifecycle(impl) || !ClientStillOwnsImpl(impl)) {
      for (size_t tail = i + 1; tail < pending.size(); tail += 1) {
        impl->pending.push_back(std::move(pending[tail]));
      }
      break;
    }
  }
}

void FailClient(LspClientImpl *impl, String8 reason, i64 error_code = kLspClientErrorProtocol) {
  if (impl->state == LspClientState::Failed || impl->state == LspClientState::Stopped) return;
  impl->failure_reason = PushStr8Copy(impl->arena, reason);
  TransportStop(impl);
  impl->state = LspClientState::Failed;
  PromoteRequestedCancellations(impl);
  DispatchCancelled(impl);
  FailPendingRequests(impl, impl->failure_reason, error_code);
}

void FinalizeStoppedClient(LspClientImpl *impl) {
  PromoteRequestedCancellations(impl);
  DispatchCancelled(impl);
  FailPendingRequests(impl, Str8Lit("client stopped"), kLspClientErrorStopped);
  TransportStop(impl);
  ClearRuntimeState(impl);
  impl->state = LspClientState::Stopped;
}

i64 JsonRpcErrorCodeOrDefault(const JsonValue *error_value, i64 fallback) {
  i64 code = fallback;
  (void)JsonGetI64(JsonObjectGet(error_value, Str8Lit("code")), &code);
  return code;
}

bool SendRequestImpl(LspClientImpl *impl, PendingRequestKind kind, String8 method,
                     String8 params_json, LspClientWriteJsonProc params_writer, void *params_user_data,
                     const LspClientRequestContext *context, LspClientResponseProc response_proc,
                     void *response_user_data, u64 *out_id) {
  if (impl == nullptr || method.size == 0) return false;
  u64 id = PeekNextRequestId(impl);

  TempArena scratch = ScratchBegin1(impl->arena);
  String8 payload = params_writer
                        ? BuildEnvelopeFromWriter(scratch.arena, true, id, method, params_writer,
                                                  params_user_data)
                        : BuildEnvelopeFromRaw(scratch.arena, true, id, method, params_json);
  if (payload.size == 0) {
    ScratchEnd(scratch);
    return false;
  }
  bool sent = TransportSend(impl, payload);
  ScratchEnd(scratch);
  if (!sent) return false;
  impl->next_request_id = id;

  PendingRequest request = {};
  request.id = id;
  request.method = std::string((const char *)method.str, (size_t)method.size);
  request.context = context ? *context : LspClientRequestContext{};
  request.response_proc = response_proc;
  request.response_user_data = response_user_data;
  request.kind = kind;
  request.cancellation_requested = false;
  impl->pending.push_back(std::move(request));
  if (out_id) *out_id = id;
  return true;
}

bool SendNotificationImpl(LspClientImpl *impl, String8 method, String8 params_json,
                          LspClientWriteJsonProc params_writer, void *params_user_data) {
  if (impl == nullptr || method.size == 0) return false;
  TempArena scratch = ScratchBegin1(impl->arena);
  String8 payload = params_writer ? BuildEnvelopeFromWriter(scratch.arena, false, 0, method,
                                                            params_writer, params_user_data)
                                  : BuildEnvelopeFromRaw(scratch.arena, false, 0, method, params_json);
  if (payload.size == 0) {
    ScratchEnd(scratch);
    return false;
  }
  bool sent = TransportSend(impl, payload);
  ScratchEnd(scratch);
  return sent;
}

PendingRequest *FindPending(LspClientImpl *impl, u64 id, size_t *out_index = nullptr) {
  for (size_t i = 0; i < impl->pending.size(); i += 1) {
    if (impl->pending[i].id == id) {
      if (out_index) *out_index = i;
      return &impl->pending[i];
    }
  }
  return nullptr;
}

bool GetResponseId(const JsonValue *value, u64 *out) {
  u64 id = 0;
  if (!JsonGetU64(value, &id) || id == 0) return false;
  *out = id;
  return true;
}

bool WriteResponseId(JsonWriter *writer, const JsonValue *id_value) {
  if (id_value == nullptr) return false;
  String8 string = {};
  i64 signed_id = 0;
  u64 unsigned_id = 0;
  if (JsonGetString(id_value, &string)) return JsonWriteString(writer, string);
  if (JsonGetI64(id_value, &signed_id)) return JsonWriteI64(writer, signed_id);
  if (JsonGetU64(id_value, &unsigned_id)) return JsonWriteU64(writer, unsigned_id);
  return false;
}

bool SendResponseObject(LspClientImpl *impl, const JsonValue *id_value,
                        auto &&body_writer) {
  TempArena scratch = ScratchBegin1(impl->arena);
  JsonWriter writer = {};
  JsonWriterInit(&writer, scratch.arena);
  bool ok = JsonWriteObjectBegin(&writer) && JsonWriteObjectKey(&writer, Str8Lit("jsonrpc")) &&
            JsonWriteString(&writer, Str8Lit("2.0")) && JsonWriteObjectKey(&writer, Str8Lit("id")) &&
            WriteResponseId(&writer, id_value) && body_writer(&writer) && JsonWriteObjectEnd(&writer);
  String8 payload = ok ? JsonWriterFinish(&writer) : String8{};
  JsonWriterDestroy(&writer);
  bool sent = payload.size != 0 && TransportSend(impl, payload);
  ScratchEnd(scratch);
  return sent;
}

bool SendNullResult(LspClientImpl *impl, const JsonValue *id_value) {
  return SendResponseObject(impl, id_value,
                            [](JsonWriter *writer) { return JsonWriteObjectKey(writer, Str8Lit("result")) &&
                                                             JsonWriteNull(writer); });
}

bool SendErrorResult(LspClientImpl *impl, const JsonValue *id_value, i64 code, String8 message) {
  return SendResponseObject(
      impl, id_value, [&](JsonWriter *writer) {
        return JsonWriteObjectKey(writer, Str8Lit("error")) && JsonWriteObjectBegin(writer) &&
               JsonWriteObjectKey(writer, Str8Lit("code")) && JsonWriteI64(writer, code) &&
               JsonWriteObjectKey(writer, Str8Lit("message")) && JsonWriteString(writer, message) &&
               JsonWriteObjectEnd(writer);
      });
}

bool SendWorkspaceFoldersResult(LspClientImpl *impl, const JsonValue *id_value) {
  return SendResponseObject(
      impl, id_value, [&](JsonWriter *writer) {
        return JsonWriteObjectKey(writer, Str8Lit("result")) && JsonWriteArrayBegin(writer) &&
               JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("uri")) &&
               JsonWriteString(writer, impl->root_uri) && JsonWriteObjectKey(writer, Str8Lit("name")) &&
               JsonWriteString(writer, impl->workspace_name) && JsonWriteObjectEnd(writer) &&
               JsonWriteArrayEnd(writer);
      });
}

bool SendConfigurationResult(LspClientImpl *impl, const JsonValue *id_value, u64 count) {
  return SendResponseObject(
      impl, id_value, [&](JsonWriter *writer) {
        bool ok = JsonWriteObjectKey(writer, Str8Lit("result")) && JsonWriteArrayBegin(writer);
        for (u64 i = 0; ok && i < count; i += 1) ok = JsonWriteNull(writer);
        return ok && JsonWriteArrayEnd(writer);
      });
}

bool SendApplyEditResult(LspClientImpl *impl, const JsonValue *id_value,
                         const LspClientApplyEditResult &result) {
  return SendResponseObject(
      impl, id_value, [&](JsonWriter *writer) {
        bool ok = JsonWriteObjectKey(writer, Str8Lit("result")) && JsonWriteObjectBegin(writer) &&
                  JsonWriteObjectKey(writer, Str8Lit("applied")) && JsonWriteBool(writer, result.applied);
        if (ok && !result.applied && result.failure_reason.size > 0) {
          ok = JsonWriteObjectKey(writer, Str8Lit("failureReason")) &&
               JsonWriteString(writer, result.failure_reason);
        }
        return ok && JsonWriteObjectEnd(writer);
      });
}

bool WriteEmptyObject(JsonWriter *writer, void *user_data) {
  (void)user_data;
  return JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer);
}

bool WriteInitializeParams(JsonWriter *writer, void *user_data) {
  LspClientImpl *impl = (LspClientImpl *)user_data;
  bool supports_workspace_edit = impl->config.callbacks.apply_edit != nullptr;
  bool ok = JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("processId")) &&
            JsonWriteNull(writer) && JsonWriteObjectKey(writer, Str8Lit("clientInfo")) &&
            JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("name")) &&
            JsonWriteString(writer, Str8Lit("1coder")) && JsonWriteObjectEnd(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("rootUri")) && JsonWriteString(writer, impl->root_uri) &&
            JsonWriteObjectKey(writer, Str8Lit("workspaceFolders")) && JsonWriteArrayBegin(writer) &&
            JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("uri")) &&
            JsonWriteString(writer, impl->root_uri) && JsonWriteObjectKey(writer, Str8Lit("name")) &&
            JsonWriteString(writer, impl->workspace_name) && JsonWriteObjectEnd(writer) &&
            JsonWriteArrayEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("capabilities")) &&
            JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("workspace")) &&
            JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("applyEdit")) &&
            JsonWriteBool(writer, supports_workspace_edit) &&
            JsonWriteObjectKey(writer, Str8Lit("configuration")) && JsonWriteBool(writer, true) &&
            JsonWriteObjectKey(writer, Str8Lit("workspaceFolders")) && JsonWriteBool(writer, true);
  if (ok && supports_workspace_edit) {
    ok = JsonWriteObjectKey(writer, Str8Lit("workspaceEdit")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("documentChanges")) && JsonWriteBool(writer, true) &&
         JsonWriteObjectKey(writer, Str8Lit("resourceOperations")) && JsonWriteArrayBegin(writer) &&
         JsonWriteString(writer, Str8Lit("create")) && JsonWriteString(writer, Str8Lit("rename")) &&
         JsonWriteString(writer, Str8Lit("delete")) && JsonWriteArrayEnd(writer) &&
         JsonWriteObjectEnd(writer);
  }
  return ok && JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("general")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("positionEncodings")) &&
         JsonWriteArrayBegin(writer) && JsonWriteString(writer, Str8Lit("utf-8")) &&
         JsonWriteString(writer, Str8Lit("utf-16")) && JsonWriteString(writer, Str8Lit("utf-32")) &&
         JsonWriteArrayEnd(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("textDocument")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("synchronization")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("didSave")) && JsonWriteBool(writer, true) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("completion")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("hover")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("declaration")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("definition")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("implementation")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("typeDefinition")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("formatting")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("rename")) && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("prepareSupport")) && JsonWriteBool(writer, true) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectKey(writer, Str8Lit("publishDiagnostics")) &&
         JsonWriteObjectBegin(writer) && JsonWriteObjectEnd(writer) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectEnd(writer) && JsonWriteObjectEnd(writer);
}

void HandleNotification(LspClientImpl *impl, String8 method, const JsonValue *params) {
  if (impl->config.callbacks.notification != nullptr) {
    ScopedCallback callback_scope(impl);
    impl->config.callbacks.notification(impl->config.callbacks.user_data, method, params);
  }
}

bool HandleServerRequest(LspClientImpl *impl, const JsonValue *root, String8 method) {
  const JsonValue *id_value = JsonObjectGet(root, Str8Lit("id"));
  if (id_value == nullptr) {
    FailClient(impl, Str8Lit("server request is missing id"));
    return false;
  }
  const JsonValue *params = JsonObjectGet(root, Str8Lit("params"));

  if (Str8Match(method, Str8Lit("workspace/configuration"))) {
    const JsonValue *items = params ? JsonObjectGet(params, Str8Lit("items")) : nullptr;
    if (items == nullptr || items->kind != JsonKind::Array) {
      FailClient(impl, Str8Lit("workspace/configuration params.items must be an array"));
      return false;
    }
    if (!SendConfigurationResult(impl, id_value, items->array.count)) {
      FailClient(impl, Str8Lit("failed to respond to workspace/configuration"),
                 kLspClientErrorTransport);
      return false;
    }
    return true;
  }

  if (Str8Match(method, Str8Lit("workspace/workspaceFolders"))) {
    if (!SendWorkspaceFoldersResult(impl, id_value)) {
      FailClient(impl, Str8Lit("failed to respond to workspace/workspaceFolders"),
                 kLspClientErrorTransport);
      return false;
    }
    return true;
  }

  if (Str8Match(method, Str8Lit("client/registerCapability")) ||
      Str8Match(method, Str8Lit("client/unregisterCapability"))) {
    if (!SendNullResult(impl, id_value)) {
      FailClient(impl, Str8Lit("failed to respond to capability request"),
                 kLspClientErrorTransport);
      return false;
    }
    return true;
  }

  if (Str8Match(method, Str8Lit("workspace/applyEdit"))) {
    const JsonValue *edit = params ? JsonObjectGet(params, Str8Lit("edit")) : nullptr;
    if (edit == nullptr) {
      FailClient(impl, Str8Lit("workspace/applyEdit params.edit is required"));
      return false;
    }

    LspClientApplyEditResult result = {};
    if (impl->config.callbacks.apply_edit != nullptr) {
      ScopedCallback callback_scope(impl);
      result = impl->config.callbacks.apply_edit(impl->config.callbacks.user_data, edit);
      if (!result.applied && result.failure_reason.size > 0) {
        result.failure_reason = PushStr8Copy(impl->arena, result.failure_reason);
      }
    } else {
      result.applied = false;
      result.failure_reason = Str8Lit("workspace/applyEdit unsupported");
    }

    if (!SendApplyEditResult(impl, id_value, result)) {
      FailClient(impl, Str8Lit("failed to respond to workspace/applyEdit"),
                 kLspClientErrorTransport);
      return false;
    }
    return true;
  }

  if (!SendErrorResult(impl, id_value, -32601, Str8Lit("Method not found"))) {
    FailClient(impl, Str8Lit("failed to respond to unknown server request"),
               kLspClientErrorTransport);
    return false;
  }
  return true;
}

bool HandleResponse(LspClientImpl *impl, const JsonValue *root) {
  u64 id = 0;
  if (!GetResponseId(JsonObjectGet(root, Str8Lit("id")), &id)) {
    FailClient(impl, Str8Lit("response id must be a non-zero integer"));
    return false;
  }

  size_t index = 0;
  PendingRequest *pending = FindPending(impl, id, &index);
  if (pending == nullptr) {
    if (ConsumeId(&impl->ignored_cancelled_ids, id)) return true;
    FailClient(impl, Str8Lit("unknown or duplicate response id"));
    return false;
  }

  JsonValue *result = JsonObjectGet(root, Str8Lit("result"));
  JsonValue *error = JsonObjectGet(root, Str8Lit("error"));
  if ((result == nullptr) == (error == nullptr)) {
    FailClient(impl, Str8Lit("response must contain exactly one of result or error"));
    return false;
  }

  PendingRequest request = *pending;
  impl->pending.erase(impl->pending.begin() + (long long)index);

  if (request.kind == PendingRequestKind::Initialize) {
    if (error != nullptr) {
      String8 message = {};
      if (!JsonGetString(JsonObjectGet(error, Str8Lit("message")), &message)) {
        message = Str8Lit("initialize request failed");
      }
      FailClient(impl, message);
      return false;
    }

    String8 decode_error = {};
    LspInitializeResult initialize = {};
    if (!LspDecodeInitializeResult(impl->arena, result, &initialize, &decode_error)) {
      FailClient(impl, decode_error);
      return false;
    }

    impl->capabilities = initialize.capabilities;
    impl->position_encoding = initialize.capabilities.position_encoding;
    if (!SendNotificationImpl(impl, Str8Lit("initialized"), String8{}, WriteEmptyObject, nullptr)) {
      FailClient(impl, Str8Lit("failed to send initialized notification"),
                 kLspClientErrorTransport);
      return false;
    }
    impl->state = LspClientState::Ready;
    return true;
  }

  if (request.kind == PendingRequestKind::Shutdown) {
    if (error != nullptr) {
      String8 message = {};
      if (!JsonGetString(JsonObjectGet(error, Str8Lit("message")), &message)) {
        message = Str8Lit("shutdown request failed");
      }
      FailClient(impl, message);
      return false;
    }
    return true;
  }

  if (request.response_proc != nullptr) {
    LspClientResponse response = {};
    response.id = request.id;
    response.method = Str8C(request.method.c_str());
    response.context = request.context;
    response.has_result = result != nullptr;
    response.result = result;
    response.has_error = error != nullptr;
    if (error != nullptr) {
      response.error_data = JsonObjectGet(error, Str8Lit("data"));
      response.error_code = JsonRpcErrorCodeOrDefault(error, kLspClientErrorProtocol);
      (void)JsonGetString(JsonObjectGet(error, Str8Lit("message")), &response.error_message);
    }
    ScopedCallback callback_scope(impl);
    request.response_proc(request.response_user_data, &response);
  }
  return true;
}

bool ProcessInboundMessage(LspClientImpl *impl, Arena *arena, const LspInboundMessage &message) {
  if (message.stderr_message) return true;

  JsonParseResult parsed = JsonParse(arena, message.json);
  if (parsed.root == nullptr) {
    FailClient(impl, PushStr8F(impl->arena, "invalid JSON from language server: %.*s",
                               (int)parsed.error.size, (char *)parsed.error.str));
    return false;
  }
  if (parsed.root->kind != JsonKind::Object) {
    FailClient(impl, Str8Lit("json-rpc payload must be an object"));
    return false;
  }

  String8 version = {};
  if (!JsonGetString(JsonObjectGet(parsed.root, Str8Lit("jsonrpc")), &version) ||
      !Str8Match(version, Str8Lit("2.0"))) {
    FailClient(impl, Str8Lit("json-rpc payload must include jsonrpc:\"2.0\""));
    return false;
  }

  JsonValue *method_value = JsonObjectGet(parsed.root, Str8Lit("method"));
  if (method_value != nullptr) {
    String8 method = {};
    if (!JsonGetString(method_value, &method)) {
      FailClient(impl, Str8Lit("json-rpc method must be a string"));
      return false;
    }
    if (JsonObjectGet(parsed.root, Str8Lit("id")) != nullptr) {
      return HandleServerRequest(impl, parsed.root, method);
    }
    HandleNotification(impl, method, JsonObjectGet(parsed.root, Str8Lit("params")));
    return true;
  }

  if (JsonObjectGet(parsed.root, Str8Lit("id")) == nullptr) {
    FailClient(impl, Str8Lit("json-rpc payload must be a request, notification, or response"));
    return false;
  }
  return HandleResponse(impl, parsed.root);
}

void PumpTransportMessages(LspClientImpl *impl) {
  TempArena scratch = ScratchBegin1(impl->arena);
  for (;;) {
    LspInboundMessage message = {};
    if (!TransportPop(impl, scratch.arena, &message)) break;
    if (!ProcessInboundMessage(impl, scratch.arena, message)) break;
    if (impl->state == LspClientState::Failed || HasDeferredLifecycle(impl)) break;
    ArenaPopTo(scratch.arena, scratch.pos);
  }
  ScratchEnd(scratch);

  if (!HasDeferredLifecycle(impl) && impl->state != LspClientState::Stopped &&
      impl->state != LspClientState::Failed && TransportFailed(impl)) {
    if (impl->state == LspClientState::Stopping) return;
    String8 reason = TransportFailureReason(impl);
    if (reason.size == 0) reason = Str8Lit("language server transport failed");
    FailClient(impl, reason, kLspClientErrorTransport);
  }
}

void StopImplNow(LspClientImpl *impl) {
  if (impl == nullptr || impl->state == LspClientState::Stopped) return;
  bool preserve_destroy = impl->destroy_deferred;
  impl->stop_requested = false;
  impl->destroy_deferred = false;

  if (impl->state == LspClientState::Ready) {
    impl->state = LspClientState::Stopping;
    if (!SendRequestImpl(impl, PendingRequestKind::Shutdown, Str8Lit("shutdown"), String8{}, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr)) {
      FinalizeStoppedClient(impl);
      impl->destroy_deferred = impl->destroy_deferred || preserve_destroy;
      return;
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(impl->config.shutdown_timeout_ms);
    auto has_shutdown_pending = [&]() {
      for (const PendingRequest &request : impl->pending) {
        if (request.kind == PendingRequestKind::Shutdown) return true;
      }
      return false;
    };
    while (has_shutdown_pending() && std::chrono::steady_clock::now() < deadline &&
           !TransportFailed(impl) && !HasDeferredLifecycle(impl)) {
      PumpTransportMessages(impl);
      std::this_thread::sleep_for(std::chrono::milliseconds(kLspClientStopPollMs));
    }

    if (!has_shutdown_pending() && !TransportFailed(impl) && !HasDeferredLifecycle(impl)) {
      (void)SendNotificationImpl(impl, Str8Lit("exit"), String8{}, nullptr, nullptr);
    }
  }

  FinalizeStoppedClient(impl);
  impl->destroy_deferred = impl->destroy_deferred || preserve_destroy;
}

void DeleteDetachedImplIfNeeded(LspClient *client, LspClientImpl *impl) {
  if (impl == nullptr || !impl->destroy_deferred || impl->tick_depth != 0 || impl->callback_depth != 0) return;
  if (client != nullptr && client->impl == impl) client->impl = nullptr;
  if (!ClientStillOwnsImpl(impl)) delete impl;
}

bool StartInternal(LspClientImpl *impl) {
  ClearRuntimeState(impl);
  impl->stderr_summary = {};
  impl->root_uri = LspPathToUri(impl->arena, impl->config.workspace_root);
  if (impl->root_uri.size == 0) {
    impl->failure_reason = PushStr8Copy(impl->arena, Str8Lit("workspace root must be an absolute path"));
    impl->state = LspClientState::Failed;
    return false;
  }
  impl->workspace_name = PushStr8Copy(impl->arena, RootName(impl->config.workspace_root));
  impl->capabilities = {};
  impl->position_encoding = LspPositionEncoding::Utf16;
  impl->state = LspClientState::Starting;

  LspTransportConfig transport_config = {};
  transport_config.command = impl->config.command;
  transport_config.wake = impl->config.wake;
  transport_config.wake_user_data = impl->config.wake_user_data;
  if (!TransportStart(impl, &transport_config)) {
    impl->failure_reason = PushStr8Copy(impl->arena, Str8Lit("failed to start language server process"));
    impl->state = LspClientState::Failed;
    return false;
  }

  u64 initialize_id = 0;
  if (!SendRequestImpl(impl, PendingRequestKind::Initialize, Str8Lit("initialize"), String8{},
                       WriteInitializeParams, impl, nullptr, nullptr, nullptr, &initialize_id)) {
    FailClient(impl, Str8Lit("failed to send initialize request"), kLspClientErrorTransport);
    return false;
  }
  impl->state = LspClientState::Initializing;
  return true;
}

}  // namespace

bool LspClientConfigure(LspClient *client, const LspClientConfig *config) {
  if (client == nullptr || config == nullptr || config->command.executable.size == 0 ||
      config->workspace_root.size == 0) {
    return false;
  }

  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr) {
    impl = new (std::nothrow) LspClientImpl();
    if (impl == nullptr || impl->arena == nullptr) {
      delete impl;
      return false;
    }
    client->impl = impl;
  } else {
    LspClientDestroy(client);
    return LspClientConfigure(client, config);
  }

  impl->config = *config;
  impl->config.command = CopyCommand(impl->arena, &config->command);
  impl->config.workspace_root = CopyString8(impl->arena, config->workspace_root);
  impl->owner = client;
  impl->runtime_pos = ArenaPos(impl->arena);
  impl->state = LspClientState::Stopped;
  impl->position_encoding = LspPositionEncoding::Utf16;
  impl->next_request_id = 0;
  impl->stderr_summary_storage.clear();
  impl->stderr_summary = {};
  if (impl->config.shutdown_timeout_ms == 0) {
    impl->config.shutdown_timeout_ms = kLspClientShutdownTimeoutMs;
  }
  return true;
}

bool LspClientStart(LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Stopped) {
    return false;
  }
  return StartInternal(impl);
}

void LspClientTick(LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr) return;
  impl->tick_depth += 1;

  DispatchCancelled(impl);
  if (ClientStillOwnsImpl(impl) && !HasDeferredLifecycle(impl) && impl->state != LspClientState::Stopped) {
    PumpTransportMessages(impl);
    if (ClientStillOwnsImpl(impl) && !HasDeferredLifecycle(impl)) {
      PromoteRequestedCancellations(impl);
      DispatchCancelled(impl);
    }
  }

  bool process_deferred = impl->tick_depth == 1 && HasDeferredLifecycle(impl);
  impl->tick_depth -= 1;
  if (impl->tick_depth == 0 && process_deferred) {
    StopImplNow(impl);
    DeleteDetachedImplIfNeeded(client, impl);
  }
}

u64 LspClientSendRequest(LspClient *client, String8 method, LspClientWriteJsonProc params_writer,
                         void *params_user_data, const LspClientRequestContext *context,
                         LspClientResponseProc response_proc, void *response_user_data) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Ready) return 0;
  u64 id = 0;
  if (!SendRequestImpl(impl, PendingRequestKind::User, method, String8{}, params_writer,
                       params_user_data, context, response_proc, response_user_data, &id)) {
    return 0;
  }
  return id;
}

u64 LspClientSendRequestJson(LspClient *client, String8 method, String8 params_json,
                             const LspClientRequestContext *context,
                             LspClientResponseProc response_proc,
                             void *response_user_data) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Ready) return 0;
  u64 id = 0;
  if (!SendRequestImpl(impl, PendingRequestKind::User, method, params_json, nullptr, nullptr,
                       context, response_proc, response_user_data, &id)) {
    return 0;
  }
  return id;
}

bool LspClientSendNotification(LspClient *client, String8 method,
                               LspClientWriteJsonProc params_writer,
                               void *params_user_data) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Ready) return false;
  return SendNotificationImpl(impl, method, String8{}, params_writer, params_user_data);
}

bool LspClientSendNotificationJson(LspClient *client, String8 method, String8 params_json) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Ready) return false;
  return SendNotificationImpl(impl, method, params_json, nullptr, nullptr);
}

bool LspClientCancel(LspClient *client, u64 request_id) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state != LspClientState::Ready || request_id == 0) return false;

  PendingRequest *pending = FindPending(impl, request_id, nullptr);
  if (pending == nullptr || pending->kind != PendingRequestKind::User) return false;

  auto write_cancel = [](JsonWriter *writer, void *user_data) {
    u64 id = *(u64 *)user_data;
    return JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("id")) &&
           JsonWriteU64(writer, id) && JsonWriteObjectEnd(writer);
  };
  if (!SendNotificationImpl(impl, Str8Lit("$/cancelRequest"), String8{}, write_cancel, &request_id)) {
    return false;
  }

  pending->cancellation_requested = true;
  return true;
}

void LspClientStop(LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || impl->state == LspClientState::Stopped) return;

  if (impl->tick_depth > 0 || impl->callback_depth > 0) {
    impl->stop_requested = true;
    return;
  }
  StopImplNow(impl);
  DeleteDetachedImplIfNeeded(client, impl);
}

void LspClientDestroy(LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr) {
    if (client) *client = {};
    return;
  }
  if (impl->tick_depth > 0 || impl->callback_depth > 0) {
    impl->stop_requested = true;
    impl->destroy_deferred = true;
    client->impl = nullptr;
    return;
  }
  StopImplNow(impl);
  delete impl;
  client->impl = nullptr;
}

LspClientState LspClientGetState(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  return impl ? impl->state : LspClientState::Stopped;
}

const LspServerCapabilities *LspClientGetCapabilities(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  return impl ? &impl->capabilities : nullptr;
}

LspPositionEncoding LspClientGetPositionEncoding(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  return impl ? impl->position_encoding : LspPositionEncoding::Utf16;
}

String8 LspClientGetFailureReason(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  return impl ? impl->failure_reason : String8{};
}

String8 LspClientGetStderrSummary(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr) return {};
  if (impl->stderr_summary.size > 0) return impl->stderr_summary;
  return TransportStderrSummary(impl);
}

bool LspClientCanRestart(const LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  return impl != nullptr && impl->state == LspClientState::Failed && !impl->restart_consumed;
}

bool LspClientRestart(LspClient *client) {
  LspClientImpl *impl = GetImpl(client);
  if (impl == nullptr || !LspClientCanRestart(client)) return false;
  impl->restart_consumed = true;
  TransportStop(impl);
  impl->state = LspClientState::Stopped;
  return StartInternal(impl);
}
