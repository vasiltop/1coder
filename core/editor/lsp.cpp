#include "editor/lsp.h"

#include "editor/editor.h"
#include "editor/lsp_ui.h"
#include "lsp/json.h"

#include <deque>
#include <new>
#include <string>

struct EditorLspSession {
  LspLanguage language;
  std::string root_storage;
  String8 root;
  LspClient client;
  LspClientState last_state = LspClientState::Stopped;
  bool failure_status_applied = false;
};

struct EditorLspDocument {
  u64 id = 0;
  BufferHandle buffer = {};
  std::string uri_storage;
  String8 uri = {};
  std::string language_id_storage;
  String8 language_id = {};
  EditorLspSession *session = nullptr;
  i64 version = 0;
  bool did_open_sent = false;
};

struct MissingServerEntry {
  LspLanguage language;
  std::string root_storage;
  String8 root;
};

struct EditorLsp {
  Editor *editor = nullptr;
  bool enabled = false;
  EditorLspConfig config = {};
  u64 next_document_id = 1;
  std::deque<EditorLspSession> sessions;
  std::deque<EditorLspDocument> documents;
  std::deque<MissingServerEntry> missing_servers;
};

namespace {

EditorLsp *GetState(Editor *ed) { return ed ? ed->lsp : nullptr; }

String8 StringView(const std::string &text) { return Str8C(text.c_str()); }

bool IsAttachableBuffer(const Buffer *buffer) {
  return buffer != nullptr && buffer->kind == BufferKind::File && buffer->path.size > 0;
}

u64 BufferIdentity(BufferHandle handle) {
  return (handle.index * 1315423911ull) ^ (handle.generation * 2654435761ull);
}

EditorLspDocument *FindDocument(EditorLsp *state, BufferHandle handle) {
  if (state == nullptr || handle.index == 0) return nullptr;
  for (EditorLspDocument &document : state->documents) {
    if (BufferHandleEqual(document.buffer, handle)) return &document;
  }
  return nullptr;
}

EditorLspDocument *FindDocument(EditorLsp *state, Buffer *buffer) {
  return (state && buffer) ? FindDocument(state, buffer->handle) : nullptr;
}

EditorLspSession *FindSession(EditorLsp *state, LspLanguage language, String8 root) {
  if (state == nullptr) return nullptr;
  for (EditorLspSession &session : state->sessions) {
    if (session.language == language && Str8Match(session.root, root)) return &session;
  }
  return nullptr;
}

bool HasMissingServer(EditorLsp *state, LspLanguage language, String8 root) {
  if (state == nullptr) return false;
  for (const MissingServerEntry &entry : state->missing_servers) {
    if (entry.language == language && Str8Match(entry.root, root)) return true;
  }
  return false;
}

void CacheMissingServer(EditorLsp *state, LspLanguage language, String8 root) {
  if (state == nullptr || HasMissingServer(state, language, root)) return;
  state->missing_servers.emplace_back();
  MissingServerEntry &entry = state->missing_servers.back();
  entry.language = language;
  entry.root_storage.assign((const char *)root.str, (size_t)root.size);
  entry.root = StringView(entry.root_storage);
}

void SetFailureStatus(Editor *ed, EditorLspSession *session, String8 fallback) {
  if (ed == nullptr || session == nullptr || session->failure_status_applied) return;

  String8 reason = LspClientGetFailureReason(&session->client);
  if (reason.size == 0) reason = LspClientGetStderrSummary(&session->client);
  if (reason.size == 0) reason = fallback;

  EditorSetStatusF(ed, "LSP: %.*s", (int)reason.size, (char *)reason.str);
  session->failure_status_applied = true;
}

bool ResolveCommand(EditorLsp *state, Arena *arena, LspLanguage language, String8 language_id,
                    String8 root, LspServerCommand *command) {
  if (state == nullptr || command == nullptr) return false;
  if (state->config.resolve_command != nullptr) {
    return state->config.resolve_command(arena, language, language_id, root, command,
                                         state->config.resolve_command_user_data);
  }
  return LspResolveServerCommand(arena, language, language_id, root, command);
}

bool WritePosition(JsonWriter *writer, LspPosition position) {
  return JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("line")) &&
         JsonWriteU64(writer, position.line) &&
         JsonWriteObjectKey(writer, Str8Lit("character")) &&
         JsonWriteU64(writer, position.character) && JsonWriteObjectEnd(writer);
}

bool WriteRange(JsonWriter *writer, LspRange range) {
  return JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("start")) &&
         WritePosition(writer, range.start) && JsonWriteObjectKey(writer, Str8Lit("end")) &&
         WritePosition(writer, range.end) && JsonWriteObjectEnd(writer);
}

struct DidOpenParams {
  Editor *ed;
  EditorLspDocument *document;
};

struct DidChangeParams {
  EditorLspDocument *document;
  bool full_text;
  LspRange range;
  String8 text;
};

struct DidSaveParams {
  Editor *ed;
  EditorLspDocument *document;
  bool include_text;
};

Buffer *ResolveDocumentBuffer(Editor *ed, const EditorLspDocument *document) {
  return (ed && document) ? BufferFromHandle(&ed->buffers, document->buffer) : nullptr;
}

String8 BufferTextForLsp(Arena *arena, const Buffer *buffer) {
  String8 text = BufferTextAll(arena, buffer);
  if (buffer != nullptr && buffer->final_newline && text.size > 0 && text.str[text.size - 1] != '\n') {
    return PushStr8Cat(arena, text, Str8Lit("\n"));
  }
  return text;
}

bool WriteDidOpen(JsonWriter *writer, void *user_data) {
  DidOpenParams *params = (DidOpenParams *)user_data;
  if (params == nullptr || params->ed == nullptr || params->document == nullptr) return false;

  Buffer *buffer = ResolveDocumentBuffer(params->ed, params->document);
  if (buffer == nullptr) return false;

  TempArena scratch = ScratchBegin();
  String8 text = BufferTextForLsp(scratch.arena, buffer);
  bool ok = JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("uri")) &&
            JsonWriteString(writer, params->document->uri) &&
            JsonWriteObjectKey(writer, Str8Lit("languageId")) &&
            JsonWriteString(writer, params->document->language_id) &&
            JsonWriteObjectKey(writer, Str8Lit("version")) &&
            JsonWriteI64(writer, params->document->version) &&
            JsonWriteObjectKey(writer, Str8Lit("text")) && JsonWriteString(writer, text) &&
            JsonWriteObjectEnd(writer) && JsonWriteObjectEnd(writer);
  ScratchEnd(scratch);
  return ok;
}

bool WriteDidChange(JsonWriter *writer, void *user_data) {
  DidChangeParams *params = (DidChangeParams *)user_data;
  if (params == nullptr || params->document == nullptr) return false;

  bool ok = JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("uri")) &&
            JsonWriteString(writer, params->document->uri) &&
            JsonWriteObjectKey(writer, Str8Lit("version")) &&
            JsonWriteI64(writer, params->document->version) &&
            JsonWriteObjectEnd(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("contentChanges")) &&
            JsonWriteArrayBegin(writer) && JsonWriteObjectBegin(writer);
  if (!ok) return false;

  if (!params->full_text) {
    ok = JsonWriteObjectKey(writer, Str8Lit("range")) && WriteRange(writer, params->range);
  }
  if (!ok) return false;

  ok = JsonWriteObjectKey(writer, Str8Lit("text")) && JsonWriteString(writer, params->text) &&
       JsonWriteObjectEnd(writer) && JsonWriteArrayEnd(writer) && JsonWriteObjectEnd(writer);
  return ok;
}

bool WriteDidSave(JsonWriter *writer, void *user_data) {
  DidSaveParams *params = (DidSaveParams *)user_data;
  if (params == nullptr || params->document == nullptr) return false;

  bool ok = JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(writer) &&
            JsonWriteObjectKey(writer, Str8Lit("uri")) &&
            JsonWriteString(writer, params->document->uri) &&
            JsonWriteObjectEnd(writer);
  if (!ok) return false;

  if (params->include_text) {
    Buffer *buffer = ResolveDocumentBuffer(params->ed, params->document);
    if (buffer == nullptr) return false;
    TempArena scratch = ScratchBegin();
    String8 text = BufferTextForLsp(scratch.arena, buffer);
    ok = JsonWriteObjectKey(writer, Str8Lit("text")) && JsonWriteString(writer, text);
    ScratchEnd(scratch);
    if (!ok) return false;
  }

  return JsonWriteObjectEnd(writer);
}

bool WriteDidClose(JsonWriter *writer, void *user_data) {
  EditorLspDocument *document = (EditorLspDocument *)user_data;
  return document != nullptr && JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("textDocument")) &&
         JsonWriteObjectBegin(writer) &&
         JsonWriteObjectKey(writer, Str8Lit("uri")) &&
         JsonWriteString(writer, document->uri) && JsonWriteObjectEnd(writer) &&
         JsonWriteObjectEnd(writer);
}

void ApplyNotification(EditorLsp *state, String8 method, const JsonValue *params) {
  if (state == nullptr || state->editor == nullptr) return;

  if (Str8Match(method, Str8Lit("textDocument/publishDiagnostics"))) {
    String8 error = {};
    if (!EditorLspUiApplyPublishDiagnostics(state->editor->lsp_ui, state->editor, params, &error)) {
      if (error.size > 0) {
        EditorSetStatusF(state->editor, "LSP: %.*s", (int)error.size, (char *)error.str);
      } else {
        EditorSetStatus(state->editor, Str8Lit("LSP: malformed diagnostics"));
      }
    }
    return;
  }

  if (Str8Match(method, Str8Lit("window/showMessage")) ||
      Str8Match(method, Str8Lit("window/logMessage"))) {
    String8 message = {};
    JsonValue *message_value = params ? JsonObjectGet(params, Str8Lit("message")) : nullptr;
    if (JsonGetString(message_value, &message) && message.size > 0) {
      EditorSetStatusF(state->editor, "LSP: %.*s", (int)message.size, (char *)message.str);
    } else {
      EditorSetStatus(state->editor, Str8Lit("LSP: malformed notification"));
    }
  }
}

void OnClientNotification(void *user_data, String8 method, const JsonValue *params) {
  ApplyNotification((EditorLsp *)user_data, method, params);
}

void EraseDocument(EditorLsp *state, BufferHandle handle) {
  if (state == nullptr || handle.index == 0) return;
  for (auto it = state->documents.begin(); it != state->documents.end(); ++it) {
    if (!BufferHandleEqual(it->buffer, handle)) continue;
    state->documents.erase(it);
    return;
  }
}

bool SendDidOpen(EditorLsp *state, EditorLspDocument *document) {
  if (state == nullptr || document == nullptr || document->session == nullptr ||
      document->did_open_sent) {
    return false;
  }
  if (LspClientGetState(&document->session->client) != LspClientState::Ready) return false;

  DidOpenParams params = {state->editor, document};
  if (LspClientSendNotification(&document->session->client, Str8Lit("textDocument/didOpen"),
                                WriteDidOpen, &params)) {
    document->did_open_sent = true;
    return true;
  }

  SetFailureStatus(state->editor, document->session, Str8Lit("failed to send didOpen"));
  return false;
}

bool SendPendingOpens(EditorLsp *state, EditorLspSession *session) {
  if (state == nullptr || session == nullptr) return false;
  bool changed = false;
  for (EditorLspDocument &document : state->documents) {
    if (document.session != session || document.did_open_sent) continue;
    changed |= SendDidOpen(state, &document);
  }
  return changed;
}

bool RestartFailedSession(EditorLsp *state, EditorLspSession *session) {
  if (state == nullptr || session == nullptr ||
      LspClientGetState(&session->client) != LspClientState::Failed) {
    return false;
  }
  if (!LspClientCanRestart(&session->client)) {
    SetFailureStatus(state->editor, session, Str8Lit("language server restart exhausted"));
    return false;
  }

  for (EditorLspDocument &document : state->documents) {
    if (document.session == session) document.did_open_sent = false;
  }
  session->failure_status_applied = false;
  if (!LspClientRestart(&session->client)) {
    session->last_state = LspClientGetState(&session->client);
    SetFailureStatus(state->editor, session, Str8Lit("failed to restart language server"));
    return false;
  }

  session->last_state = LspClientGetState(&session->client);
  EditorSetStatus(state->editor, Str8Lit("LSP: language server restarting; retry action"));
  return true;
}

void SendDidClose(EditorLsp *state, EditorLspDocument *document) {
  if (state == nullptr || document == nullptr || document->session == nullptr ||
      !document->did_open_sent) {
    return;
  }
  if (LspClientGetState(&document->session->client) != LspClientState::Ready) return;
  if (!LspClientSendNotification(&document->session->client, Str8Lit("textDocument/didClose"),
                                 WriteDidClose, document)) {
    SetFailureStatus(state->editor, document->session, Str8Lit("failed to send didClose"));
  }
}

void StopAndClear(EditorLsp *state) {
  if (state == nullptr) return;

  for (EditorLspDocument &document : state->documents) SendDidClose(state, &document);
  state->documents.clear();

  for (EditorLspSession &session : state->sessions) {
    LspClientStop(&session.client);
    LspClientDestroy(&session.client);
  }
  state->sessions.clear();
  state->missing_servers.clear();
  EditorLspUiReset(state->editor ? state->editor->lsp_ui : nullptr);
}

EditorLspSession *CreateSession(EditorLsp *state, LspLanguage language, String8 language_id,
                                String8 root) {
  if (state == nullptr) return nullptr;

  TempArena scratch = ScratchBegin();
  LspServerCommand command = {};
  bool have_command = ResolveCommand(state, scratch.arena, language, language_id, root, &command);
  if (!have_command) {
    CacheMissingServer(state, language, root);
    EditorSetStatusF(state->editor, "LSP: no server for %.*s", (int)language_id.size,
                     (char *)language_id.str);
    ScratchEnd(scratch);
    return nullptr;
  }

  state->sessions.emplace_back();
  EditorLspSession &session = state->sessions.back();
  session.language = language;
  session.root_storage.assign((const char *)root.str, (size_t)root.size);
  session.root = StringView(session.root_storage);

  LspClientConfig config = {};
  config.command = command;
  config.workspace_root = root;
  config.wake = state->config.wake;
  config.wake_user_data = state->config.wake_user_data;
  config.callbacks.notification = OnClientNotification;
  config.callbacks.user_data = state;
  config.shutdown_timeout_ms = state->config.shutdown_timeout_ms;
  if (!LspClientConfigure(&session.client, &config)) {
    state->sessions.pop_back();
    EditorSetStatus(state->editor, Str8Lit("LSP: failed to configure client"));
    ScratchEnd(scratch);
    return nullptr;
  }

  if (!LspClientStart(&session.client)) {
    SetFailureStatus(state->editor, &session, Str8Lit("failed to start language server process"));
  }
  session.last_state = LspClientGetState(&session.client);

  ScratchEnd(scratch);
  return &session;
}

EditorLspSession *SessionForBuffer(EditorLsp *state, String8 path, String8 language_id,
                                   LspLanguage language) {
  TempArena scratch = ScratchBegin();
  String8 root = LspFindProjectRoot(scratch.arena, language, path);
  if (HasMissingServer(state, language, root)) {
    ScratchEnd(scratch);
    return nullptr;
  }
  EditorLspSession *session = FindSession(state, language, root);
  if (session == nullptr) session = CreateSession(state, language, language_id, root);
  ScratchEnd(scratch);
  return session;
}

EditorLspDocument *EnsureDocument(EditorLsp *state, Buffer *buffer) {
  if (state == nullptr || buffer == nullptr) return nullptr;
  if (!IsAttachableBuffer(buffer)) return nullptr;

  if (EditorLspDocument *existing = FindDocument(state, buffer)) return existing;

  TempArena scratch = ScratchBegin();
  LspLanguage language = {};
  String8 language_id = {};
  if (!LspLanguageForPath(buffer->path, &language, &language_id)) {
    ScratchEnd(scratch);
    return nullptr;
  }

  String8 uri = LspPathToUri(scratch.arena, buffer->path);
  if (uri.size == 0) {
    ScratchEnd(scratch);
    return nullptr;
  }

  EditorLspSession *session = SessionForBuffer(state, buffer->path, language_id, language);
  if (session == nullptr) {
    ScratchEnd(scratch);
    return nullptr;
  }

  state->documents.emplace_back();
  EditorLspDocument &document = state->documents.back();
  document.id = state->next_document_id++;
  document.buffer = buffer->handle;
  document.uri_storage.assign((const char *)uri.str, (size_t)uri.size);
  document.uri = StringView(document.uri_storage);
  document.language_id_storage.assign((const char *)language_id.str, (size_t)language_id.size);
  document.language_id = StringView(document.language_id_storage);
  document.session = session;
  document.version = (i64)buffer->edit_serial;

  if (LspClientGetState(&session->client) == LspClientState::Ready) SendDidOpen(state, &document);
  ScratchEnd(scratch);
  return &document;
}

bool FillBufferInfo(Editor *ed, Buffer *buffer, EditorLspBufferInfo *out) {
  EditorLsp *state = GetState(ed);
  EditorLspDocument *document = FindDocument(state, buffer);
  if (document == nullptr || document->session == nullptr) return false;

  if (out != nullptr) {
    out->document = document;
    out->session = document->session;
    out->client = &document->session->client;
    out->capabilities = LspClientGetCapabilities(&document->session->client);
    out->position_encoding = LspClientGetPositionEncoding(&document->session->client);
    out->version = document->version;
    out->did_open_sent = document->did_open_sent;
  }
  return true;
}

void SendFullDidChange(Editor *ed, EditorLspDocument *document, Buffer *buffer) {
  if (ed == nullptr || document == nullptr || buffer == nullptr || document->session == nullptr ||
      !document->did_open_sent) {
    return;
  }
  if (LspClientGetState(&document->session->client) != LspClientState::Ready) return;

  TempArena scratch = ScratchBegin();
  String8 text = BufferTextForLsp(scratch.arena, buffer);
  DidChangeParams params = {document, true, {}, text};
  if (!LspClientSendNotification(&document->session->client, Str8Lit("textDocument/didChange"),
                                 WriteDidChange, &params)) {
    SetFailureStatus(ed, document->session, Str8Lit("failed to send didChange"));
  }
  ScratchEnd(scratch);
}

}  // namespace

void EditorLspInit(Editor *ed) {
  if (ed == nullptr) return;
  ed->lsp = new (std::nothrow) EditorLsp();
  if (ed->lsp != nullptr) ed->lsp->editor = ed;
}

void EditorLspDestroy(Editor *ed) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr) return;
  StopAndClear(state);
  delete state;
  ed->lsp = nullptr;
}

void EditorLspEnable(Editor *ed, const EditorLspConfig *config) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr) return;

  StopAndClear(state);
  state->enabled = true;
  state->config = config ? *config : EditorLspConfig{};

  for (BufferHandle handle = BufferFirst(&ed->buffers); handle.index != 0;
       handle = BufferNext(&ed->buffers, handle)) {
    Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
    if (IsAttachableBuffer(buffer)) (void)EnsureDocument(state, buffer);
  }
}

void EditorLspDisable(Editor *ed) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr) return;
  StopAndClear(state);
  state->enabled = false;
  state->config = {};
}

bool EditorLspTick(Editor *ed) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr || !state->enabled) return false;

  bool changed = false;
  for (EditorLspSession &session : state->sessions) {
    LspClientState before = LspClientGetState(&session.client);
    LspClientTick(&session.client);
    LspClientState after = LspClientGetState(&session.client);
    if (after != before) {
      changed = true;
      session.last_state = after;
      if (after == LspClientState::Ready) {
        changed |= SendPendingOpens(state, &session);
      } else if (after == LspClientState::Failed) {
        SetFailureStatus(ed, &session, Str8Lit("language server failed"));
      }
    } else if (after == LspClientState::Ready) {
      changed |= SendPendingOpens(state, &session);
    }
  }
  return changed;
}

void EditorLspOnFileBufferOpened(Editor *ed, Buffer *buffer) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr || !state->enabled || !IsAttachableBuffer(buffer)) return;
  (void)EnsureDocument(state, buffer);
}

void EditorLspBeforeBufferEdit(Editor *ed, Buffer *buffer, RangeU64 old_range, String8 new_text,
                               i64 next_version) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr || !state->enabled || buffer == nullptr) return;

  EditorLspDocument *document = EnsureDocument(state, buffer);
  if (document == nullptr) return;
  document->version = next_version;

  if (!document->did_open_sent || document->session == nullptr ||
      LspClientGetState(&document->session->client) != LspClientState::Ready) {
    return;
  }

  LspPositionEncoding encoding = LspClientGetPositionEncoding(&document->session->client);
  DidChangeParams params = {};
  params.document = document;
  params.full_text = false;
  params.range.start = LspPositionFromOffset(buffer, old_range.min, encoding);
  params.range.end = LspPositionFromOffset(buffer, old_range.max, encoding);
  params.text = new_text;

  if (!LspClientSendNotification(&document->session->client, Str8Lit("textDocument/didChange"),
                                 WriteDidChange, &params)) {
    SetFailureStatus(ed, document->session, Str8Lit("failed to send didChange"));
  }
}

void EditorLspAfterBufferReset(Editor *ed, Buffer *buffer) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr || !state->enabled || buffer == nullptr) return;

  EditorLspDocument *document = EnsureDocument(state, buffer);
  if (document == nullptr) return;
  document->version = (i64)buffer->edit_serial;

  if (!document->did_open_sent) return;
  SendFullDidChange(ed, document, buffer);
}

void EditorLspOnBufferSaved(Editor *ed, Buffer *buffer) {
  EditorLsp *state = GetState(ed);
  EditorLspDocument *document = FindDocument(state, buffer);
  if (state == nullptr || document == nullptr || document->session == nullptr ||
      !document->did_open_sent) {
    return;
  }

  TempArena scratch = ScratchBegin();
  String8 current_uri = LspPathToUri(scratch.arena, buffer->path);
  if (current_uri.size > 0 && !Str8Match(current_uri, document->uri)) {
    BufferHandle handle = document->buffer;
    SendDidClose(state, document);
    EraseDocument(state, handle);
    document = EnsureDocument(state, buffer);
  }
  ScratchEnd(scratch);
  if (document == nullptr || document->session == nullptr || !document->did_open_sent) return;
  if (LspClientGetState(&document->session->client) != LspClientState::Ready) return;

  const LspServerCapabilities *capabilities = LspClientGetCapabilities(&document->session->client);
  DidSaveParams params = {ed, document,
                          capabilities && capabilities->text_document_sync.save_include_text};
  if (!LspClientSendNotification(&document->session->client, Str8Lit("textDocument/didSave"),
                                 WriteDidSave, &params)) {
    SetFailureStatus(ed, document->session, Str8Lit("failed to send didSave"));
  }
}

void EditorLspOnBufferClosed(Editor *ed, BufferHandle handle) {
  EditorLsp *state = GetState(ed);
  if (state == nullptr || !state->enabled) return;

  EditorLspDocument *document = FindDocument(state, handle);
  if (document == nullptr) return;
  SendDidClose(state, document);
  EraseDocument(state, handle);
}

bool EditorLspGetBufferInfo(Editor *ed, Buffer *buffer, EditorLspBufferInfo *out) {
  return FillBufferInfo(ed, buffer, out);
}

u64 EditorLspSendRequestJson(Editor *ed, Buffer *buffer, String8 method, String8 params_json,
                             u32 request_kind, void *request_token,
                             LspClientResponseProc response_proc, void *response_user_data) {
  EditorLsp *state = GetState(ed);
  EditorLspDocument *document = FindDocument(state, buffer);
  if (document == nullptr || document->session == nullptr) return 0;
  if (LspClientGetState(&document->session->client) == LspClientState::Failed) {
    (void)RestartFailedSession(state, document->session);
    return 0;
  }
  if (!document->did_open_sent) return 0;

  LspClientRequestContext context = {};
  context.document_id = document->id;
  context.buffer_id = BufferIdentity(document->buffer);
  context.document_version = document->version;
  context.request_kind = request_kind;
  context.request_token = request_token;
  return LspClientSendRequestJson(&document->session->client, method, params_json, &context,
                                  response_proc, response_user_data);
}

bool EditorLspSendNotificationJson(Editor *ed, Buffer *buffer, String8 method,
                                   String8 params_json) {
  EditorLsp *state = GetState(ed);
  EditorLspDocument *document = FindDocument(state, buffer);
  if (document == nullptr || document->session == nullptr || !document->did_open_sent) return false;
  return LspClientSendNotificationJson(&document->session->client, method, params_json);
}

void EditorLspApplyClientFailureStatus(Editor *ed, Buffer *buffer) {
  EditorLsp *state = GetState(ed);
  EditorLspDocument *document = FindDocument(state, buffer);
  if (document == nullptr || document->session == nullptr) return;
  if (LspClientGetState(&document->session->client) == LspClientState::Failed) {
    (void)RestartFailedSession(state, document->session);
  }
}
