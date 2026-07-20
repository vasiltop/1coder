#pragma once

#include "base/base_string.h"
#include "base/base_types.h"
#include "lsp/protocol.h"
#include "lsp/registry.h"
#include "lsp/transport.h"

struct JsonValue;
struct JsonWriter;

struct LspClient {
  void *impl;
};

enum class LspClientState : u8 { Stopped, Starting, Initializing, Ready, Failed, Stopping };

inline constexpr i64 kLspClientErrorCancelled = -32800;
inline constexpr i64 kLspClientErrorTransport = -32098;
inline constexpr i64 kLspClientErrorProtocol = -32097;
inline constexpr i64 kLspClientErrorStopped = -32096;

struct LspClientRequestContext {
  u64 document_id;
  u64 buffer_id;
  i64 document_version;
  u32 request_kind;
  void *request_token;
};

struct LspClientResponse {
  u64 id;
  String8 method;
  LspClientRequestContext context;
  bool cancelled;
  bool has_result;
  const JsonValue *result;      // Valid only for the duration of the callback.
  bool has_error;
  i64 error_code;
  String8 error_message;
  const JsonValue *error_data;  // Valid only for the duration of the callback.
};

using LspClientResponseProc = void (*)(void *user_data, const LspClientResponse *response);
using LspClientNotificationProc = void (*)(void *user_data, String8 method,
                                           const JsonValue *params);

struct LspClientApplyEditResult {
  bool applied;
  String8 failure_reason;
};

using LspClientApplyEditProc = LspClientApplyEditResult (*)(void *user_data,
                                                            const JsonValue *edit);
using LspClientWriteJsonProc = bool (*)(JsonWriter *writer, void *user_data);

struct LspClientCallbacks {
  LspClientNotificationProc notification;
  LspClientApplyEditProc apply_edit;
  void *user_data;
};

struct LspClientTransportHooks {
  bool (*start)(void *user_data, const LspTransportConfig *config);
  bool (*send)(void *user_data, String8 json);
  bool (*pop)(void *user_data, Arena *arena, LspInboundMessage *message);
  void (*stop)(void *user_data);
  bool (*failed)(void *user_data);
  String8 (*failure_reason)(void *user_data);
  String8 (*stderr_summary)(void *user_data);
};

struct LspClientConfig {
  LspServerCommand command;
  String8 workspace_root;
  LspWakeProc wake;
  void *wake_user_data;
  LspClientCallbacks callbacks;
  const LspClientTransportHooks *transport_hooks;
  void *transport_user_data;
  u32 shutdown_timeout_ms;
};

[[nodiscard]] bool LspClientConfigure(LspClient *client, const LspClientConfig *config);
[[nodiscard]] bool LspClientStart(LspClient *client);
void LspClientTick(LspClient *client);

[[nodiscard]] u64 LspClientSendRequest(LspClient *client, String8 method,
                                       LspClientWriteJsonProc params_writer,
                                       void *params_user_data,
                                       const LspClientRequestContext *context,
                                       LspClientResponseProc response_proc,
                                       void *response_user_data);
[[nodiscard]] u64 LspClientSendRequestJson(LspClient *client, String8 method, String8 params_json,
                                           const LspClientRequestContext *context,
                                           LspClientResponseProc response_proc,
                                           void *response_user_data);
[[nodiscard]] bool LspClientSendNotification(LspClient *client, String8 method,
                                             LspClientWriteJsonProc params_writer,
                                             void *params_user_data);
[[nodiscard]] bool LspClientSendNotificationJson(LspClient *client, String8 method,
                                                 String8 params_json);
[[nodiscard]] bool LspClientCancel(LspClient *client, u64 request_id);
void LspClientStop(LspClient *client);
void LspClientDestroy(LspClient *client);

[[nodiscard]] LspClientState LspClientGetState(const LspClient *client);
[[nodiscard]] const LspServerCapabilities *LspClientGetCapabilities(
    const LspClient *client);
[[nodiscard]] LspPositionEncoding LspClientGetPositionEncoding(const LspClient *client);
[[nodiscard]] String8 LspClientGetFailureReason(const LspClient *client);
[[nodiscard]] String8 LspClientGetStderrSummary(const LspClient *client);
[[nodiscard]] bool LspClientCanRestart(const LspClient *client);
[[nodiscard]] bool LspClientRestart(LspClient *client);
