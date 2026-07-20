#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "lsp/registry.h"

struct LspTransport {
  void *impl;
};

using LspWakeProc = void (*)(void *user_data);

struct LspTransportConfig {
  LspServerCommand command;
  LspWakeProc wake;
  void *wake_user_data;
};

struct LspInboundMessage {
  String8 json;
  bool stderr_message;
};

[[nodiscard]] bool LspTransportStart(LspTransport *transport, const LspTransportConfig *config);
[[nodiscard]] bool LspTransportSend(LspTransport *transport, String8 json);
[[nodiscard]] bool LspTransportPop(LspTransport *transport, Arena *arena, LspInboundMessage *message);
[[nodiscard]] bool LspTransportFailed(const LspTransport *transport);
[[nodiscard]] String8 LspTransportFailureReason(const LspTransport *transport);
[[nodiscard]] String8 LspTransportStderrSummary(const LspTransport *transport);
[[nodiscard]] String8 LspTransportStopAndCaptureStderr(LspTransport *transport, Arena *arena);
void LspTransportStop(LspTransport *transport);
