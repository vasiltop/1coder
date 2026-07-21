#pragma once

#include "editor/buffer.h"
#include "lsp/client.h"
#include "lsp/protocol.h"
#include "lsp/registry.h"

struct Editor;
struct EditorLsp;
struct EditorLspDocument;
struct EditorLspSession;

struct EditorLspConfig {
  LspWakeProc wake;
  void *wake_user_data;
  u32 shutdown_timeout_ms;
  bool (*resolve_command)(Arena *arena, LspLanguage language, String8 language_id, String8 root,
                          LspServerCommand *command, void *user_data);
  void *resolve_command_user_data;
};

struct EditorLspBufferInfo {
  const EditorLspDocument *document;
  const EditorLspSession *session;
  LspClient *client;
  const LspServerCapabilities *capabilities;
  LspPositionEncoding position_encoding;
  i64 version;
  bool did_open_sent;
};

void EditorLspInit(Editor *ed);
void EditorLspDestroy(Editor *ed);
void EditorLspEnable(Editor *ed, const EditorLspConfig *config);
void EditorLspDisable(Editor *ed);
// Re-runs enable with the current config (clears missing-server cache and
// reattaches open files). Used after :config-reload changes LSP overrides.
void EditorLspRestartSessions(Editor *ed);
[[nodiscard]] bool EditorLspIsEnabled(Editor *ed);

[[nodiscard]] bool EditorLspTick(Editor *ed);

void EditorLspOnFileBufferOpened(Editor *ed, Buffer *buffer);
void EditorLspBeforeBufferEdit(Editor *ed, Buffer *buffer, RangeU64 old_range, String8 new_text,
                               i64 next_version);
void EditorLspAfterBufferReset(Editor *ed, Buffer *buffer);
void EditorLspOnBufferSaved(Editor *ed, Buffer *buffer);
void EditorLspOnBufferClosed(Editor *ed, BufferHandle handle);

[[nodiscard]] bool EditorLspGetBufferInfo(Editor *ed, Buffer *buffer, EditorLspBufferInfo *out);

[[nodiscard]] u64 EditorLspSendRequestJson(Editor *ed, Buffer *buffer, String8 method,
                                           String8 params_json, u32 request_kind,
                                           void *request_token,
                                           LspClientResponseProc response_proc,
                                           void *response_user_data);
[[nodiscard]] bool EditorLspSendNotificationJson(Editor *ed, Buffer *buffer, String8 method,
                                                 String8 params_json);

void EditorLspApplyClientFailureStatus(Editor *ed, Buffer *buffer);
