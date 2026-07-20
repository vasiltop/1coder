#include "editor/lsp_actions.h"

#include "editor/editor.h"
#include "editor/filetype.h"
#include "editor/lsp.h"
#include "editor/view.h"
#include "lsp/json.h"
#include "os/os_file.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace {

struct ValidatedTextEdit {
  u64 start = 0;
  u64 end = 0;
  u64 order = 0;
  String8 new_text = {};
};

struct WorkspaceTarget {
  String8 path = {};
  BufferHandle handle = {};
  Buffer *buffer = nullptr;
  bool opened_now = false;
  bool has_version = false;
  i64 version = 0;
  std::vector<LspTextEdit> edits;
  std::vector<ValidatedTextEdit> validated;
};

String8 FinishWriter(JsonWriter *writer) {
  String8 json = JsonWriterFinish(writer);
  if (json.size == 0) return {};
  return json;
}

void SetStatus(Editor *ed, String8 detail) {
  if (ed == nullptr) return;
  EditorSetStatusF(ed, "LSP: %.*s", (int)detail.size, (char *)detail.str);
}

void SetStatus(Editor *ed, const char *detail) {
  if (ed == nullptr) return;
  EditorSetStatusF(ed, "LSP: %s", detail);
}

bool CopyOwnedString(String8 source, String8 *out) {
  if (out == nullptr) return false;
  *out = {};
  if (source.size == 0) return true;
  u8 *copy = new (std::nothrow) u8[(size_t)source.size];
  if (copy == nullptr) return false;
  memcpy(copy, source.str, (size_t)source.size);
  *out = String8{copy, source.size};
  return true;
}

template <typename T>
void DestroyOwnedContext(T *context) {
  if (context == nullptr || !context->owned) return;
  delete[] context->uri.str;
  delete context;
}

template <typename T>
struct OwnedContextCleanup {
  T *context;
  ~OwnedContextCleanup() { DestroyOwnedContext(context); }
};

String8 CurrentBufferUri(Arena *arena, Buffer *buffer) {
  if (arena == nullptr || buffer == nullptr || buffer->path.size == 0) return {};
  return LspPathToUri(arena, buffer->path);
}

Buffer *ResolveRequestBuffer(Editor *ed, BufferHandle handle, String8 uri, i64 version,
                             bool *stale) {
  if (stale) *stale = false;
  if (ed == nullptr) return nullptr;

  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (buffer == nullptr) {
    if (stale) *stale = true;
    return nullptr;
  }

  TempArena scratch = ScratchBegin();
  String8 current_uri = CurrentBufferUri(scratch.arena, buffer);
  bool current = current_uri.size > 0 && Str8Match(current_uri, uri) &&
                 (version < 0 || (i64)buffer->edit_serial == version);
  ScratchEnd(scratch);
  if (!current) {
    if (stale) *stale = true;
    return nullptr;
  }

  return buffer;
}

bool WritePosition(JsonWriter *writer, LspPosition position) {
  return JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("line")) &&
         JsonWriteU64(writer, position.line) &&
         JsonWriteObjectKey(writer, Str8Lit("character")) &&
         JsonWriteU64(writer, position.character) && JsonWriteObjectEnd(writer);
}

bool BuildPositionParams(Arena *arena, String8 uri, LspPosition position, String8 *out) {
  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);
  bool ok = JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("uri")) && JsonWriteString(&writer, uri) &&
            JsonWriteObjectEnd(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("position")) &&
            WritePosition(&writer, position) && JsonWriteObjectEnd(&writer);
  *out = ok ? FinishWriter(&writer) : String8{};
  JsonWriterDestroy(&writer);
  return out->size > 0;
}

bool BuildFormattingParams(Arena *arena, String8 uri, String8 *out) {
  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);
  bool ok = JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("uri")) && JsonWriteString(&writer, uri) &&
            JsonWriteObjectEnd(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("options")) &&
            JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("tabSize")) && JsonWriteU64(&writer, 2) &&
            JsonWriteObjectKey(&writer, Str8Lit("insertSpaces")) &&
            JsonWriteBool(&writer, true) && JsonWriteObjectEnd(&writer) &&
            JsonWriteObjectEnd(&writer);
  *out = ok ? FinishWriter(&writer) : String8{};
  JsonWriterDestroy(&writer);
  return out->size > 0;
}

bool BuildRenameParams(Arena *arena, String8 uri, LspPosition position, String8 new_name,
                       String8 *out) {
  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);
  bool ok = JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("textDocument")) &&
            JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("uri")) && JsonWriteString(&writer, uri) &&
            JsonWriteObjectEnd(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("position")) &&
            WritePosition(&writer, position) &&
            JsonWriteObjectKey(&writer, Str8Lit("newName")) &&
            JsonWriteString(&writer, new_name) && JsonWriteObjectEnd(&writer);
  *out = ok ? FinishWriter(&writer) : String8{};
  JsonWriterDestroy(&writer);
  return out->size > 0;
}

String8 NavigationMethod(EditorLspNavigationKind kind) {
  switch (kind) {
    case EditorLspNavigationKind::Declaration: return Str8Lit("textDocument/declaration");
    case EditorLspNavigationKind::Definition: return Str8Lit("textDocument/definition");
    case EditorLspNavigationKind::Implementation: return Str8Lit("textDocument/implementation");
    case EditorLspNavigationKind::TypeDefinition: return Str8Lit("textDocument/typeDefinition");
  }
  return {};
}

String8 NavigationCapabilityStatus(EditorLspNavigationKind kind) {
  switch (kind) {
    case EditorLspNavigationKind::Declaration: return Str8Lit("declaration unsupported");
    case EditorLspNavigationKind::Definition: return Str8Lit("definition unsupported");
    case EditorLspNavigationKind::Implementation: return Str8Lit("implementation unsupported");
    case EditorLspNavigationKind::TypeDefinition: return Str8Lit("type definition unsupported");
  }
  return Str8Lit("navigation unsupported");
}

bool NavigationSupported(const LspServerCapabilities *capabilities,
                         EditorLspNavigationKind kind) {
  if (capabilities == nullptr) return false;
  switch (kind) {
    case EditorLspNavigationKind::Declaration: return capabilities->declaration_provider;
    case EditorLspNavigationKind::Definition: return capabilities->definition_provider;
    case EditorLspNavigationKind::Implementation: return capabilities->implementation_provider;
    case EditorLspNavigationKind::TypeDefinition: return capabilities->type_definition_provider;
  }
  return false;
}

bool PrepareTrackedRequest(Editor *ed, Buffer *buffer, EditorLspBufferInfo *info, String8 *uri) {
  if (ed == nullptr || buffer == nullptr) return false;
  if (!EditorLspGetBufferInfo(ed, buffer, info) || info->client == nullptr || !info->did_open_sent) {
    SetStatus(ed, "file not open");
    return false;
  }
  LspClientState state = LspClientGetState(info->client);
  if (state == LspClientState::Failed) {
    EditorLspApplyClientFailureStatus(ed, buffer);
    return false;
  }
  if (state != LspClientState::Ready) {
    SetStatus(ed, "client not ready");
    return false;
  }

  TempArena scratch = ScratchBegin();
  String8 current_uri = CurrentBufferUri(scratch.arena, buffer);
  bool ok = current_uri.size > 0;
  if (ok && uri != nullptr) *uri = PushStr8Copy(ed->arena, current_uri);
  ScratchEnd(scratch);
  if (!ok) SetStatus(ed, "invalid URI");
  return ok;
}

bool PreparePositionRequest(Editor *ed, Buffer *buffer, EditorLspBufferInfo *info, String8 *uri,
                            LspPosition *position) {
  if (!PrepareTrackedRequest(ed, buffer, info, uri)) return false;
  View *view = EditorFocusedView(ed);
  if (view == nullptr || !BufferHandleEqual(view->buffer, buffer->handle)) {
    SetStatus(ed, "no cursor");
    return false;
  }
  *position = LspPositionFromOffset(buffer, view->cursor, info->position_encoding);
  return true;
}

template <typename T>
T *AllocateRequestContext(Editor *ed, Buffer *buffer, String8 uri) {
  T *context = new (std::nothrow) T{};
  if (context == nullptr) {
    SetStatus(ed, "out of memory");
    return nullptr;
  }
  context->ed = ed;
  context->buffer = buffer->handle;
  context->owned = true;
  if (!CopyOwnedString(uri, &context->uri)) {
    DestroyOwnedContext(context);
    SetStatus(ed, "out of memory");
    return nullptr;
  }
  return context;
}

bool TryOffsetFromPosition(Buffer *buffer, LspPosition position, LspPositionEncoding encoding,
                           u64 *out) {
  if (buffer == nullptr || out == nullptr) return false;
  u64 offset = LspOffsetFromPosition(buffer, position, encoding);
  LspPosition roundtrip = LspPositionFromOffset(buffer, offset, encoding);
  if (roundtrip.line != position.line || roundtrip.character != position.character) return false;
  *out = offset;
  return true;
}

bool CollectValidatedTextEdits(Editor *ed, Buffer *buffer, LspPositionEncoding position_encoding,
                               const LspTextEdit *edits, u64 edit_count,
                               std::vector<ValidatedTextEdit> *out) {
  if (buffer == nullptr || out == nullptr) return false;
  out->clear();
  out->reserve((size_t)edit_count);
  for (u64 i = 0; i < edit_count; i += 1) {
    u64 start = 0;
    u64 end = 0;
    if (!TryOffsetFromPosition(buffer, edits[i].range.start, position_encoding, &start) ||
        !TryOffsetFromPosition(buffer, edits[i].range.end, position_encoding, &end) ||
        end < start) {
      SetStatus(ed, "malformed result");
      return false;
    }
    out->push_back(ValidatedTextEdit{start, end, i, edits[i].new_text});
  }

  std::sort(out->begin(), out->end(), [](const ValidatedTextEdit &a, const ValidatedTextEdit &b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.end != b.end) return a.end < b.end;
    return a.order < b.order;
  });
  for (u64 i = 1; i < edit_count; i += 1) {
    if ((*out)[i].start < (*out)[i - 1].end) {
      SetStatus(ed, "overlapping edits");
      return false;
    }
  }
  return true;
}

u64 CursorHint(Editor *ed, Buffer *buffer) {
  View *view = EditorFocusedView(ed);
  if (view != nullptr && buffer != nullptr && BufferHandleEqual(view->buffer, buffer->handle)) {
    return Min(view->cursor, BufferSize(buffer));
  }
  return 0;
}

void ClampFocusedView(Editor *ed, Buffer *buffer) {
  View *view = EditorFocusedView(ed);
  if (ed == nullptr || view == nullptr || buffer == nullptr ||
      !BufferHandleEqual(view->buffer, buffer->handle)) {
    return;
  }
  ViewSetCursor(view, buffer, Min(view->cursor, BufferSize(buffer)));
  EditorScrollFocusedToCursor(ed);
}

void ApplyValidatedTextEdits(Editor *ed, Buffer *buffer,
                             const std::vector<ValidatedTextEdit> &validated) {
  u64 cursor = CursorHint(ed, buffer);
  BufferBeginEditGroup(buffer);
  for (auto it = validated.rbegin(); it != validated.rend(); ++it) {
    BufferReplace(ed, buffer, RangeU64{it->start, it->end}, it->new_text, cursor, cursor);
  }
  BufferEndEditGroup(buffer);
  ClampFocusedView(ed, buffer);
}

bool ResolveFallbackRename(Editor *ed, Buffer *buffer, Arena *arena,
                           EditorLspRenamePrepareResult *out) {
  if (ed == nullptr || buffer == nullptr || arena == nullptr || out == nullptr) return false;
  View *view = EditorFocusedView(ed);
  if (view == nullptr || !BufferHandleEqual(view->buffer, buffer->handle)) {
    SetStatus(ed, "no cursor");
    out->status = EditorLspRenamePrepareStatus::Failed;
    return false;
  }

  u64 size = BufferSize(buffer);
  if (view->cursor >= size) {
    SetStatus(ed, "no word under cursor");
    out->status = EditorLspRenamePrepareStatus::Failed;
    return false;
  }

  TempArena scratch = ScratchBegin();
  String8 text = BufferTextAll(scratch.arena, buffer);
  u64 start = view->cursor;
  while (start < text.size && text.str[start] != '\n' && !CharIsWord(text.str[start])) start += 1;
  if (start >= text.size || text.str[start] == '\n') {
    ScratchEnd(scratch);
    SetStatus(ed, "no word under cursor");
    out->status = EditorLspRenamePrepareStatus::Failed;
    return false;
  }

  while (start > 0 && CharIsWord(text.str[start - 1])) start -= 1;
  u64 end = start;
  while (end < text.size && CharIsWord(text.str[end])) end += 1;

  out->status = EditorLspRenamePrepareStatus::Ready;
  out->range = RangeU64{start, end};
  out->placeholder = PushStr8Copy(arena, String8{text.str + start, end - start});
  out->prompt = out->placeholder;
  ScratchEnd(scratch);
  return true;
}

void EmitPrepareRename(EditorLspRenamePrepareContext *context,
                       const EditorLspRenamePrepareResult *result) {
  if (context != nullptr && context->callback != nullptr) {
    context->callback(context->callback_user_data, result);
  }
}

void RollbackOpenedTargets(Editor *ed, std::vector<WorkspaceTarget> *targets) {
  if (ed == nullptr || targets == nullptr) return;
  for (WorkspaceTarget &target : *targets) {
    if (!target.opened_now || target.handle.index == 0) continue;
    BufferClose(&ed->buffers, ed, target.handle);
    target.handle = {};
    target.buffer = nullptr;
  }
}

WorkspaceTarget *FindTarget(std::vector<WorkspaceTarget> *targets, String8 path) {
  for (WorkspaceTarget &target : *targets) {
    if (Str8Match(target.path, path)) return &target;
  }
  return nullptr;
}

bool OpenWorkspaceTargets(Editor *ed, std::vector<WorkspaceTarget> *targets) {
  for (WorkspaceTarget &target : *targets) {
    target.handle = BufferFromPath(&ed->buffers, target.path);
    if (target.handle.index == 0) {
      if (!OsFileExists(target.path)) {
        SetStatus(ed, "open failed");
        RollbackOpenedTargets(ed, targets);
        return false;
      }
      target.handle = FiletypeOpen(ed, target.path);
      target.opened_now = true;
    }

    target.buffer = BufferFromHandle(&ed->buffers, target.handle);
    if (target.buffer == nullptr || target.buffer->kind != BufferKind::File ||
        !Str8Match(target.buffer->path, target.path)) {
      SetStatus(ed, "open failed");
      RollbackOpenedTargets(ed, targets);
      return false;
    }
  }
  return true;
}

}  // namespace

u64 EditorLspRequestFormatting(Editor *ed, Buffer *buffer) {
  EditorLspBufferInfo info = {};
  String8 uri = {};
  if (!PrepareTrackedRequest(ed, buffer, &info, &uri)) return 0;
  if (info.capabilities == nullptr || !info.capabilities->document_formatting_provider) {
    SetStatus(ed, "formatting unsupported");
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params = {};
  bool built = BuildFormattingParams(scratch.arena, uri, &params);
  EditorLspFormatContext *context =
      built ? AllocateRequestContext<EditorLspFormatContext>(ed, buffer, uri) : nullptr;
  if (!built || context == nullptr) {
    if (built) DestroyOwnedContext(context);
    ScratchEnd(scratch);
    SetStatus(ed, "request failed");
    return 0;
  }
  context->position_encoding = info.position_encoding;
  u64 id = EditorLspSendRequestJson(ed, buffer, Str8Lit("textDocument/formatting"), params, 0,
                                    nullptr, EditorLspOnFormattingResponse, context);
  ScratchEnd(scratch);
  if (id == 0) {
    DestroyOwnedContext(context);
    SetStatus(ed, "request failed");
  }
  return id;
}

u64 EditorLspRequestNavigation(Editor *ed, Buffer *buffer, EditorLspNavigationKind kind) {
  EditorLspBufferInfo info = {};
  String8 uri = {};
  LspPosition position = {};
  if (!PreparePositionRequest(ed, buffer, &info, &uri, &position)) return 0;
  if (!NavigationSupported(info.capabilities, kind)) {
    SetStatus(ed, NavigationCapabilityStatus(kind));
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params = {};
  bool built = BuildPositionParams(scratch.arena, uri, position, &params);
  EditorLspNavigationContext *context =
      built ? AllocateRequestContext<EditorLspNavigationContext>(ed, buffer, uri) : nullptr;
  if (!built || context == nullptr) {
    if (built) DestroyOwnedContext(context);
    ScratchEnd(scratch);
    SetStatus(ed, "request failed");
    return 0;
  }
  context->kind = kind;
  context->position_encoding = info.position_encoding;
  u64 id = EditorLspSendRequestJson(ed, buffer, NavigationMethod(kind), params, 0, nullptr,
                                    EditorLspOnNavigationResponse, context);
  ScratchEnd(scratch);
  if (id == 0) {
    DestroyOwnedContext(context);
    SetStatus(ed, "request failed");
  }
  return id;
}

u64 EditorLspPrepareRename(Editor *ed, Buffer *buffer, EditorLspRenamePrepareProc callback,
                           void *callback_user_data) {
  EditorLspBufferInfo info = {};
  String8 uri = {};
  LspPosition position = {};
  if (!PreparePositionRequest(ed, buffer, &info, &uri, &position)) {
    EditorLspRenamePrepareResult result = {};
    result.status = EditorLspRenamePrepareStatus::Failed;
    if (callback != nullptr) callback(callback_user_data, &result);
    return 0;
  }
  if (info.capabilities == nullptr || !info.capabilities->rename_provider) {
    SetStatus(ed, "rename unsupported");
    EditorLspRenamePrepareResult result = {};
    result.status = EditorLspRenamePrepareStatus::Unsupported;
    if (callback != nullptr) callback(callback_user_data, &result);
    return 0;
  }
  if (!info.capabilities->rename_prepare_provider) {
    EditorLspRenamePrepareResult result = {};
    TempArena scratch = ScratchBegin();
    ResolveFallbackRename(ed, buffer, scratch.arena, &result);
    if (callback != nullptr) callback(callback_user_data, &result);
    ScratchEnd(scratch);
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params = {};
  bool built = BuildPositionParams(scratch.arena, uri, position, &params);
  EditorLspRenamePrepareContext *context =
      built ? AllocateRequestContext<EditorLspRenamePrepareContext>(ed, buffer, uri) : nullptr;
  if (!built || context == nullptr) {
    if (built) DestroyOwnedContext(context);
    ScratchEnd(scratch);
    SetStatus(ed, "request failed");
    EditorLspRenamePrepareResult result = {};
    result.status = EditorLspRenamePrepareStatus::Failed;
    if (callback != nullptr) callback(callback_user_data, &result);
    return 0;
  }
  context->callback = callback;
  context->callback_user_data = callback_user_data;
  context->position_encoding = info.position_encoding;
  u64 id = EditorLspSendRequestJson(ed, buffer, Str8Lit("textDocument/prepareRename"), params, 0,
                                    nullptr, EditorLspOnPrepareRenameResponse, context);
  ScratchEnd(scratch);
  if (id == 0) {
    DestroyOwnedContext(context);
    SetStatus(ed, "request failed");
    EditorLspRenamePrepareResult result = {};
    result.status = EditorLspRenamePrepareStatus::Failed;
    if (callback != nullptr) callback(callback_user_data, &result);
  }
  return id;
}

u64 EditorLspSubmitRename(Editor *ed, Buffer *buffer, String8 new_name) {
  EditorLspBufferInfo info = {};
  String8 uri = {};
  LspPosition position = {};
  if (!PreparePositionRequest(ed, buffer, &info, &uri, &position)) return 0;
  if (info.capabilities == nullptr || !info.capabilities->rename_provider) {
    SetStatus(ed, "rename unsupported");
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params = {};
  bool built = BuildRenameParams(scratch.arena, uri, position, new_name, &params);
  EditorLspRenameContext *context =
      built ? AllocateRequestContext<EditorLspRenameContext>(ed, buffer, uri) : nullptr;
  if (!built || context == nullptr) {
    if (built) DestroyOwnedContext(context);
    ScratchEnd(scratch);
    SetStatus(ed, "request failed");
    return 0;
  }
  context->position_encoding = info.position_encoding;
  u64 id = EditorLspSendRequestJson(ed, buffer, Str8Lit("textDocument/rename"), params, 0, nullptr,
                                    EditorLspOnRenameResponse, context);
  ScratchEnd(scratch);
  if (id == 0) {
    DestroyOwnedContext(context);
    SetStatus(ed, "request failed");
  }
  return id;
}

void EditorLspOnFormattingResponse(void *user_data, const LspClientResponse *response) {
  EditorLspFormatContext *context = (EditorLspFormatContext *)user_data;
  OwnedContextCleanup<EditorLspFormatContext> cleanup = {context};
  if (context == nullptr || response == nullptr || context->ed == nullptr) return;
  if (response->cancelled) {
    SetStatus(context->ed, "cancelled");
    return;
  }
  if (response->has_error) {
    SetStatus(context->ed, response->error_message.size ? response->error_message
                                                        : Str8Lit("request failed"));
    return;
  }

  bool stale = false;
  Buffer *buffer = ResolveRequestBuffer(context->ed, context->buffer, context->uri,
                                        response->context.document_version, &stale);
  if (buffer == nullptr) {
    if (stale) SetStatus(context->ed, "stale response");
    return;
  }
  if (!response->has_result || response->result == nullptr ||
      response->result->kind == JsonKind::Null) {
    SetStatus(context->ed, "no edits");
    return;
  }

  TempArena scratch = ScratchBegin();
  LspTextEdit *edits = nullptr;
  u64 count = 0;
  String8 error = {};
  bool ok = LspDecodeTextEdits(scratch.arena, response->result, &edits, &count, &error);
  if (!ok) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    return;
  }
  if (count == 0) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "no edits");
    return;
  }
  ok = EditorLspApplyTextEdits(context->ed, buffer, -1, context->position_encoding, edits, count);
  ScratchEnd(scratch);
  if (ok) SetStatus(context->ed, "formatted");
}

void EditorLspOnNavigationResponse(void *user_data, const LspClientResponse *response) {
  EditorLspNavigationContext *context = (EditorLspNavigationContext *)user_data;
  OwnedContextCleanup<EditorLspNavigationContext> cleanup = {context};
  if (context == nullptr || response == nullptr || context->ed == nullptr) return;
  if (response->cancelled) {
    SetStatus(context->ed, "cancelled");
    return;
  }
  if (response->has_error) {
    SetStatus(context->ed, response->error_message.size ? response->error_message
                                                        : Str8Lit("request failed"));
    return;
  }

  bool stale = false;
  if (ResolveRequestBuffer(context->ed, context->buffer, context->uri,
                           response->context.document_version, &stale) == nullptr) {
    if (stale) SetStatus(context->ed, "stale response");
    return;
  }
  if (!response->has_result || response->result == nullptr) {
    SetStatus(context->ed, "no results");
    return;
  }

  TempArena scratch = ScratchBegin();
  LspLocation *locations = nullptr;
  u64 count = 0;
  String8 error = {};
  bool ok = LspDecodeLocationResult(scratch.arena, response->result, &locations, &count, &error);
  if (!ok) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    return;
  }
  if (count == 0) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "no results");
    return;
  }

  String8 path = LspUriToPath(scratch.arena, locations[0].uri);
  if (path.size == 0) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "invalid URI");
    return;
  }

  BufferHandle existing = BufferFromPath(&context->ed->buffers, path);
  BufferHandle handle = existing;
  bool opened_now = false;
  if (handle.index == 0) {
    if (!OsFileExists(path)) {
      ScratchEnd(scratch);
      SetStatus(context->ed, "open failed");
      return;
    }
    handle = FiletypeOpen(context->ed, path);
    opened_now = true;
  }

  Buffer *target = BufferFromHandle(&context->ed->buffers, handle);
  u64 offset = 0;
  LspPositionEncoding encoding = context->position_encoding;
  EditorLspBufferInfo target_info = {};
  if (target != nullptr && EditorLspGetBufferInfo(context->ed, target, &target_info)) {
    encoding = target_info.position_encoding;
  }
  if (target == nullptr || target->kind != BufferKind::File) {
    if (opened_now && handle.index != 0) BufferClose(&context->ed->buffers, context->ed, handle);
    ScratchEnd(scratch);
    SetStatus(context->ed, "open failed");
    return;
  }
  if (!TryOffsetFromPosition(target, locations[0].range.start, encoding, &offset)) {
    if (opened_now && handle.index != 0) BufferClose(&context->ed->buffers, context->ed, handle);
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    return;
  }

  View *view = EditorFocusedView(context->ed);
  if (view == nullptr) {
    if (opened_now && handle.index != 0) BufferClose(&context->ed->buffers, context->ed, handle);
    ScratchEnd(scratch);
    SetStatus(context->ed, "open failed");
    return;
  }

  EditorPushJump(context->ed, view);
  EditorShowBuffer(context->ed, handle);
  View *shown_view = EditorFocusedView(context->ed);
  Buffer *shown = BufferFromHandle(&context->ed->buffers, handle);
  if (shown_view != nullptr && shown != nullptr) {
    ViewSetCursor(shown_view, shown, offset);
    EditorScrollFocusedToCursor(context->ed);
  }
  if (count > 1) EditorSetStatusF(context->ed, "LSP: %llu results", (unsigned long long)count);
  ScratchEnd(scratch);
}

void EditorLspOnPrepareRenameResponse(void *user_data, const LspClientResponse *response) {
  EditorLspRenamePrepareContext *context = (EditorLspRenamePrepareContext *)user_data;
  OwnedContextCleanup<EditorLspRenamePrepareContext> cleanup = {context};
  if (context == nullptr || response == nullptr || context->ed == nullptr) return;

  EditorLspRenamePrepareResult result = {};
  if (response->cancelled) {
    SetStatus(context->ed, "cancelled");
    result.status = EditorLspRenamePrepareStatus::Failed;
    EmitPrepareRename(context, &result);
    return;
  }
  if (response->has_error) {
    SetStatus(context->ed, response->error_message.size ? response->error_message
                                                        : Str8Lit("request failed"));
    result.status = EditorLspRenamePrepareStatus::Failed;
    EmitPrepareRename(context, &result);
    return;
  }

  bool stale = false;
  Buffer *buffer = ResolveRequestBuffer(context->ed, context->buffer, context->uri,
                                        response->context.document_version, &stale);
  if (buffer == nullptr) {
    if (stale) SetStatus(context->ed, "stale response");
    result.status = EditorLspRenamePrepareStatus::Failed;
    EmitPrepareRename(context, &result);
    return;
  }

  TempArena scratch = ScratchBegin();
  const JsonValue *value = response->has_result ? response->result : nullptr;
  JsonValue *default_behavior = value ? JsonObjectGet(value, Str8Lit("defaultBehavior")) : nullptr;
  if (default_behavior != nullptr) {
    bool allow_default = false;
    if (!JsonGetBool(default_behavior, &allow_default)) {
      ScratchEnd(scratch);
      SetStatus(context->ed, "malformed result");
      result.status = EditorLspRenamePrepareStatus::Failed;
      EmitPrepareRename(context, &result);
      return;
    }
    if (!allow_default) {
      ScratchEnd(scratch);
      SetStatus(context->ed, "rename rejected");
      result.status = EditorLspRenamePrepareStatus::Rejected;
      EmitPrepareRename(context, &result);
      return;
    }
    ResolveFallbackRename(context->ed, buffer, scratch.arena, &result);
    EmitPrepareRename(context, &result);
    ScratchEnd(scratch);
    return;
  }

  if (value == nullptr || value->kind == JsonKind::Null) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "rename rejected");
    result.status = EditorLspRenamePrepareStatus::Rejected;
    EmitPrepareRename(context, &result);
    return;
  }

  LspRange range = {};
  String8 error = {};
  if (JsonObjectGet(value, Str8Lit("range")) != nullptr) {
    if (!LspDecodeRange(scratch.arena, JsonObjectGet(value, Str8Lit("range")), &range, &error)) {
      ScratchEnd(scratch);
      SetStatus(context->ed, "malformed result");
      result.status = EditorLspRenamePrepareStatus::Failed;
      EmitPrepareRename(context, &result);
      return;
    }
    JsonValue *placeholder = JsonObjectGet(value, Str8Lit("placeholder"));
    if (placeholder != nullptr && !JsonGetString(placeholder, &result.placeholder)) {
      ScratchEnd(scratch);
      SetStatus(context->ed, "malformed result");
      result.status = EditorLspRenamePrepareStatus::Failed;
      EmitPrepareRename(context, &result);
      return;
    }
  } else if (!LspDecodeRange(scratch.arena, value, &range, &error)) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    result.status = EditorLspRenamePrepareStatus::Failed;
    EmitPrepareRename(context, &result);
    return;
  }

  u64 start = 0;
  u64 end = 0;
  if (!TryOffsetFromPosition(buffer, range.start, context->position_encoding, &start) ||
      !TryOffsetFromPosition(buffer, range.end, context->position_encoding, &end) || end < start) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    result.status = EditorLspRenamePrepareStatus::Failed;
    EmitPrepareRename(context, &result);
    return;
  }

  result.status = EditorLspRenamePrepareStatus::Ready;
  result.range = RangeU64{start, end};
  if (result.placeholder.size == 0) {
    result.placeholder = BufferTextRange(scratch.arena, buffer, result.range);
  }
  result.prompt = result.placeholder;
  EmitPrepareRename(context, &result);
  ScratchEnd(scratch);
}

void EditorLspOnRenameResponse(void *user_data, const LspClientResponse *response) {
  EditorLspRenameContext *context = (EditorLspRenameContext *)user_data;
  OwnedContextCleanup<EditorLspRenameContext> cleanup = {context};
  if (context == nullptr || response == nullptr || context->ed == nullptr) return;
  if (response->cancelled) {
    SetStatus(context->ed, "cancelled");
    return;
  }
  if (response->has_error) {
    SetStatus(context->ed, response->error_message.size ? response->error_message
                                                        : Str8Lit("request failed"));
    return;
  }

  bool stale = false;
  if (ResolveRequestBuffer(context->ed, context->buffer, context->uri,
                           response->context.document_version, &stale) == nullptr) {
    if (stale) SetStatus(context->ed, "stale response");
    return;
  }
  if (!response->has_result || response->result == nullptr ||
      response->result->kind == JsonKind::Null) {
    SetStatus(context->ed, "no changes");
    return;
  }

  TempArena scratch = ScratchBegin();
  LspWorkspaceEdit edit = {};
  String8 error = {};
  bool ok = LspDecodeWorkspaceEdit(scratch.arena, response->result, &edit, &error);
  if (!ok) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "malformed result");
    return;
  }
  if (!edit.present || edit.count == 0) {
    ScratchEnd(scratch);
    SetStatus(context->ed, "no changes");
    return;
  }
  ok = EditorLspApplyWorkspaceEdit(context->ed, &edit, context->position_encoding);
  ScratchEnd(scratch);
  if (ok) SetStatus(context->ed, "renamed");
}

bool EditorLspApplyTextEdits(Editor *ed, Buffer *buffer, i64 expected_version,
                             LspPositionEncoding position_encoding, const LspTextEdit *edits,
                             u64 edit_count) {
  if (ed == nullptr || buffer == nullptr) return false;
  if (expected_version >= 0 && (i64)buffer->edit_serial != expected_version) {
    SetStatus(ed, "wrong version");
    return false;
  }

  std::vector<ValidatedTextEdit> validated;
  if (!CollectValidatedTextEdits(ed, buffer, position_encoding, edits, edit_count, &validated)) {
    return false;
  }

  ApplyValidatedTextEdits(ed, buffer, validated);
  return true;
}

bool EditorLspApplyWorkspaceEdit(Editor *ed, const LspWorkspaceEdit *edit,
                                 LspPositionEncoding position_encoding) {
  if (ed == nullptr || edit == nullptr || !edit->present) return true;

  TempArena scratch = ScratchBegin();
  std::vector<WorkspaceTarget> targets;
  targets.reserve((size_t)edit->count);

  for (u64 i = 0; i < edit->count; i += 1) {
    const LspWorkspaceEditEntry &entry = edit->entries[i];
    if (entry.kind != LspWorkspaceEditEntryKind::TextDocumentEdits) {
      ScratchEnd(scratch);
      SetStatus(ed, "unsupported resource op");
      return false;
    }

    String8 path = LspUriToPath(scratch.arena, entry.uri);
    if (path.size == 0) {
      ScratchEnd(scratch);
      SetStatus(ed, "invalid URI");
      return false;
    }

    WorkspaceTarget *target = FindTarget(&targets, path);
    if (target == nullptr) {
      targets.push_back({});
      target = &targets.back();
      target->path = PushStr8Copy(scratch.arena, path);
    }
    if (entry.has_version) {
      if (target->has_version && target->version != entry.version) {
        ScratchEnd(scratch);
        SetStatus(ed, "wrong version");
        return false;
      }
      target->has_version = true;
      target->version = entry.version;
    }
    target->edits.insert(target->edits.end(), entry.edits, entry.edits + entry.edit_count);
  }

  if (!OpenWorkspaceTargets(ed, &targets)) {
    ScratchEnd(scratch);
    return false;
  }

  for (WorkspaceTarget &target : targets) {
    if (target.has_version && (i64)target.buffer->edit_serial != target.version) {
      RollbackOpenedTargets(ed, &targets);
      ScratchEnd(scratch);
      SetStatus(ed, "wrong version");
      return false;
    }
    if (!CollectValidatedTextEdits(ed, target.buffer, position_encoding, target.edits.data(),
                                   (u64)target.edits.size(), &target.validated)) {
      RollbackOpenedTargets(ed, &targets);
      ScratchEnd(scratch);
      return false;
    }
  }

  for (WorkspaceTarget &target : targets) {
    ApplyValidatedTextEdits(ed, target.buffer, target.validated);
  }

  ScratchEnd(scratch);
  return true;
}
