#include "editor/lsp_ui.h"

#include "editor/editor.h"
#include "editor/lsp.h"
#include "editor/view.h"
#include "lsp/json.h"

#include <algorithm>
#include <new>
#include <vector>

struct EditorLspUiDocument {
  Arena *arena = nullptr;
  String8 uri = {};
  LspDiagnostic *diagnostics = nullptr;
  u64 diagnostic_count = 0;
  bool has_version = false;
  i64 version = 0;
  LspPositionEncoding encoding = LspPositionEncoding::Utf16;
};

struct PendingRequest {
  Editor *editor = nullptr;
  BufferHandle buffer = {};
  u64 request_id = 0;
  u64 anchor_offset = 0;
  i64 anchor_version = 0;
  LspPositionEncoding encoding = LspPositionEncoding::Utf16;
};

struct EditorLspUi {
  std::vector<EditorLspUiDocument> documents;
  Arena *popup_arena = nullptr;
  EditorLspUiPopupView popup = {};
  LspPositionEncoding popup_encoding = LspPositionEncoding::Utf16;
  PendingRequest pending_completion = {};
  PendingRequest pending_hover = {};
};

namespace {

constexpr u64 kDocumentArenaReserve = MB(1);
constexpr u64 kPopupArenaReserve = MB(1);
constexpr u64 kMaxCompletionItems = 128;
constexpr u64 kMaxCompletionLabelBytes = 256;
constexpr u64 kMaxCompletionDetailBytes = 256;
constexpr u64 kMaxCompletionFilterBytes = 256;
constexpr u64 kMaxCompletionSortBytes = 256;
constexpr u64 kMaxCompletionInsertBytes = 4096;
constexpr u64 kMaxAdditionalCompletionEdits = 32;
constexpr u64 kMaxPopupLines = 200;
constexpr u64 kMaxPopupLineBytes = 512;
constexpr u32 kCompletionRequestKind = 1;
constexpr u32 kHoverRequestKind = 2;

[[nodiscard]] u64 ClampUtf8Bytes(String8 text, u64 limit) {
  if (text.size <= limit) return text.size;
  u64 used = 0;
  for (u64 offset = 0; offset < text.size;) {
    DecodedCodepoint decoded = Utf8Decode(text, offset);
    if (used + decoded.advance > limit) break;
    used += decoded.advance;
    offset += decoded.advance;
  }
  return used;
}

String8 CopyBounded(Arena *arena, String8 text, u64 max_bytes = ~0ull) {
  if (max_bytes != ~0ull) text = Str8Prefix(text, ClampUtf8Bytes(text, max_bytes));
  return PushStr8Copy(arena, text);
}

bool PositionLess(LspPosition a, LspPosition b) {
  return (a.line < b.line) || (a.line == b.line && a.character < b.character);
}

bool PositionLessOrEqual(LspPosition a, LspPosition b) {
  return a.line == b.line ? a.character <= b.character : a.line < b.line;
}

bool RangeContainsPosition(LspRange range, LspPosition position) {
  if (range.start.line == range.end.line && range.start.character == range.end.character) {
    return range.start.line == position.line && range.start.character == position.character;
  }
  return PositionLessOrEqual(range.start, position) && PositionLess(position, range.end);
}

bool RangesOverlap(RangeU64 a, RangeU64 b) {
  if (a.min == a.max && b.min == b.max) return a.min == b.min;
  return a.min < b.max && b.min < a.max;
}

u64 RangeLength(RangeU64 range) { return range.max >= range.min ? range.max - range.min : 0; }

String8 CanonicalUri(Arena *arena, String8 uri) {
  if (uri.size == 0) return {};
  String8 path = LspUriToPath(arena, uri);
  if (path.size == 0) return PushStr8Copy(arena, uri);
  String8 canonical = LspPathToUri(arena, path);
  return canonical.size > 0 ? canonical : PushStr8Copy(arena, uri);
}

String8 CanonicalUriForBuffer(Arena *arena, Buffer *buffer) {
  if (buffer == nullptr || buffer->path.size == 0) return {};
  return LspPathToUri(arena, buffer->path);
}

EditorLspUiDocument *FindDocument(EditorLspUi *ui, String8 canonical_uri) {
  if (ui == nullptr || canonical_uri.size == 0) return nullptr;
  for (EditorLspUiDocument &document : ui->documents) {
    if (Str8Match(document.uri, canonical_uri)) return &document;
  }
  return nullptr;
}

void ReleaseDocument(EditorLspUiDocument *document) {
  if (document == nullptr) return;
  if (document->arena != nullptr) ArenaRelease(document->arena);
  *document = {};
}

EditorLspUiDocument *EnsureDocument(EditorLspUi *ui, String8 canonical_uri) {
  EditorLspUiDocument *existing = FindDocument(ui, canonical_uri);
  if (existing != nullptr) return existing;

  ui->documents.emplace_back();
  EditorLspUiDocument &document = ui->documents.back();
  document.arena = ArenaAlloc(kDocumentArenaReserve);
  document.uri = PushStr8Copy(document.arena, canonical_uri);
  return &document;
}

Buffer *FindBufferForUri(Editor *ed, String8 canonical_uri) {
  if (ed == nullptr || canonical_uri.size == 0) return nullptr;
  TempArena scratch = ScratchBegin();
  String8 path = LspUriToPath(scratch.arena, canonical_uri);
  BufferHandle handle = path.size > 0 ? BufferFromPath(&ed->buffers, path) : BufferHandleZero();
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  ScratchEnd(scratch);
  return buffer;
}

LspPositionEncoding InferEncoding(Editor *ed, Buffer *buffer, LspPositionEncoding fallback) {
  EditorLspBufferInfo info = {};
  if (ed != nullptr && buffer != nullptr && EditorLspGetBufferInfo(ed, buffer, &info)) {
    return info.position_encoding;
  }
  return fallback;
}

void ClearPopup(EditorLspUi *ui) {
  if (ui == nullptr) return;
  if (ui->popup_arena != nullptr) ArenaClear(ui->popup_arena);
  ui->popup = {};
  ui->popup_encoding = LspPositionEncoding::Utf16;
}

void ClearPending(PendingRequest *pending) {
  if (pending == nullptr) return;
  *pending = {};
}

void ResetDocuments(EditorLspUi *ui) {
  if (ui == nullptr) return;
  for (EditorLspUiDocument &document : ui->documents) ReleaseDocument(&document);
  ui->documents.clear();
}

bool SetPopupText(EditorLspUi *ui, EditorLspUiPopupKind kind, Buffer *buffer, u64 anchor_offset,
                  i64 anchor_version, bool has_range, LspRange range, const String8 *raw_lines,
                  u64 raw_count) {
  if (ui == nullptr || buffer == nullptr) return false;

  ClearPopup(ui);
  ui->popup.kind = kind;
  ui->popup.anchor_buffer = buffer->handle;
  ui->popup.anchor_offset = anchor_offset;
  ui->popup.anchor_version = anchor_version;
  ui->popup.text.has_range = has_range;
  ui->popup.text.range = range;

  if (raw_count == 0) return true;

  String8 *stored = PushArray(ui->popup_arena, String8, kMaxPopupLines);
  u64 stored_count = 0;
  bool truncated = false;
  for (u64 i = 0; i < raw_count; i += 1) {
    bool reserve_for_marker = truncated || (i + 1 < raw_count);
    if (reserve_for_marker && stored_count == kMaxPopupLines - 1) {
      truncated = true;
      break;
    }
    if (stored_count == kMaxPopupLines) {
      truncated = true;
      break;
    }

    String8 line = raw_lines[i];
    if (line.size > 0 && line.str[line.size - 1] == '\r') line = Str8Chop(line, 1);
    u64 clamped = ClampUtf8Bytes(line, kMaxPopupLineBytes);
    if (clamped < line.size) {
      line = Str8Prefix(line, clamped);
      truncated = true;
    }
    stored[stored_count++] = PushStr8Copy(ui->popup_arena, line);
  }

  if (truncated) {
    if (stored_count == kMaxPopupLines) stored_count = kMaxPopupLines - 1;
    stored[stored_count++] = PushStr8Copy(ui->popup_arena, Str8Lit("[truncated]"));
  }

  ui->popup.text.lines = stored;
  ui->popup.text.line_count = stored_count;
  return true;
}

bool SetPopupTextFromString(EditorLspUi *ui, EditorLspUiPopupKind kind, Buffer *buffer,
                            u64 anchor_offset, i64 anchor_version, bool has_range, LspRange range,
                            String8 text) {
  if (text.size == 0) {
    ClearPopup(ui);
    return true;
  }

  TempArena scratch = ScratchBegin();
  String8 *lines = PushArray(scratch.arena, String8, text.size + 1);
  u64 count = 0;
  u64 line_start = 0;
  for (u64 i = 0; i <= text.size; i += 1) {
    if (i != text.size && text.str[i] != '\n') continue;
    lines[count++] = Str8Substr(text, RangeU64{line_start, i});
    line_start = i + 1;
  }
  bool ok = SetPopupText(ui, kind, buffer, anchor_offset, anchor_version, has_range, range, lines, count);
  ScratchEnd(scratch);
  return ok;
}

void CopyTextEdit(Arena *arena, const LspTextEdit &src, LspTextEdit *dst) {
  *dst = {};
  dst->range = src.range;
  dst->new_text = CopyBounded(arena, src.new_text, kMaxCompletionInsertBytes);
}

void CopyCompletionItem(Arena *arena, const LspCompletionItem &src, EditorLspUiCompletionItem *dst) {
  *dst = {};
  dst->label = CopyBounded(arena, src.label, kMaxCompletionLabelBytes);
  dst->detail = CopyBounded(arena, src.detail, kMaxCompletionDetailBytes);
  dst->filter_text = CopyBounded(arena, src.filter_text, kMaxCompletionFilterBytes);
  dst->sort_text = CopyBounded(arena, src.sort_text, kMaxCompletionSortBytes);
  dst->insert_text = CopyBounded(arena, src.insert_text, kMaxCompletionInsertBytes);
  dst->text_edit_kind = src.text_edit_kind;
  CopyTextEdit(arena, src.text_edit, &dst->text_edit);
  dst->insert_replace_edit.insert = src.insert_replace_edit.insert;
  dst->insert_replace_edit.replace = src.insert_replace_edit.replace;
  dst->insert_replace_edit.new_text =
      CopyBounded(arena, src.insert_replace_edit.new_text, kMaxCompletionInsertBytes);

  dst->additional_text_edit_count = Min(src.additional_text_edit_count, kMaxAdditionalCompletionEdits);
  if (dst->additional_text_edit_count > 0) {
    dst->additional_text_edits = PushArray(arena, LspTextEdit, dst->additional_text_edit_count);
    for (u64 i = 0; i < dst->additional_text_edit_count; i += 1) {
      CopyTextEdit(arena, src.additional_text_edits[i], &dst->additional_text_edits[i]);
    }
  }
}

int CompareLex(String8 a, String8 b) {
  u64 count = Min(a.size, b.size);
  for (u64 i = 0; i < count; i += 1) {
    if (a.str[i] < b.str[i]) return -1;
    if (a.str[i] > b.str[i]) return 1;
  }
  if (a.size < b.size) return -1;
  if (a.size > b.size) return 1;
  return 0;
}

void MaybeSortCompletionItems(EditorLspUiCompletionItem *items, u64 count) {
  bool any_sort_text = false;
  for (u64 i = 0; i < count; i += 1) {
    if (items[i].sort_text.size > 0) {
      any_sort_text = true;
      break;
    }
  }
  if (!any_sort_text) return;

  std::stable_sort(items, items + count, [](const EditorLspUiCompletionItem &a,
                                            const EditorLspUiCompletionItem &b) {
    if (a.sort_text.size == 0 && b.sort_text.size == 0) return false;
    if (a.sort_text.size == 0) return false;
    if (b.sort_text.size == 0) return true;
    return CompareLex(a.sort_text, b.sort_text) < 0;
  });
}

const char *SeverityName(LspDiagnosticSeverity severity) {
  switch (severity) {
    case LspDiagnosticSeverity::Error: return "error";
    case LspDiagnosticSeverity::Warning: return "warning";
    case LspDiagnosticSeverity::Information: return "info";
    case LspDiagnosticSeverity::Hint: return "hint";
  }
  return "diagnostic";
}

RangeU64 ResolvePrimaryEditRange(const EditorLspUiCompletionItem &item, Buffer *buffer,
                                 u64 anchor_offset, LspPositionEncoding encoding, String8 *text) {
  *text = item.insert_text.size > 0 ? item.insert_text : item.label;
  switch (item.text_edit_kind) {
    case LspCompletionTextEditKind::TextEdit:
      *text = item.text_edit.new_text;
      return RangeU64{LspOffsetFromPosition(buffer, item.text_edit.range.start, encoding),
                      LspOffsetFromPosition(buffer, item.text_edit.range.end, encoding)};
    case LspCompletionTextEditKind::InsertReplace:
      *text = item.insert_replace_edit.new_text;
      return RangeU64{
          LspOffsetFromPosition(buffer, item.insert_replace_edit.replace.start, encoding),
          LspOffsetFromPosition(buffer, item.insert_replace_edit.replace.end, encoding)};
    case LspCompletionTextEditKind::None:
    default: return RangeU64{anchor_offset, anchor_offset};
  }
}

bool ResolveEditRange(Buffer *buffer, const LspTextEdit &edit, LspPositionEncoding encoding,
                      RangeU64 *range) {
  if (buffer == nullptr || range == nullptr) return false;
  range->min = LspOffsetFromPosition(buffer, edit.range.start, encoding);
  range->max = LspOffsetFromPosition(buffer, edit.range.end, encoding);
  return range->max >= range->min;
}

bool CompletionAccept(EditorLspUi *ui, Editor *ed, View *view) {
  if (ui == nullptr || ed == nullptr || view == nullptr) return false;
  if (ui->popup.kind != EditorLspUiPopupKind::Completion || ui->popup.completion.count == 0) return false;

  Buffer *buffer = EditorBufferForView(ed, view);
  if (buffer == nullptr || !BufferHandleEqual(buffer->handle, ui->popup.anchor_buffer) ||
      (i64)buffer->edit_serial != ui->popup.anchor_version) {
    EditorSetStatus(ed, Str8Lit("LSP: completion is stale"));
    ClearPopup(ui);
    return false;
  }

  const EditorLspUiCompletionItem &item = ui->popup.completion.items[ui->popup.completion.selected];
  String8 primary_text = {};
  RangeU64 primary_range =
      ResolvePrimaryEditRange(item, buffer, ui->popup.anchor_offset, ui->popup_encoding, &primary_text);
  if (primary_range.max < primary_range.min) {
    EditorSetStatus(ed, Str8Lit("LSP: invalid completion edit"));
    ClearPopup(ui);
    return false;
  }

  struct PendingEdit {
    RangeU64 range;
    String8 text;
  };

  u64 edit_count = item.additional_text_edit_count + 1;
  PendingEdit *edits = PushArray(ui->popup_arena, PendingEdit, edit_count);
  edits[0] = PendingEdit{primary_range, primary_text};
  for (u64 i = 0; i < item.additional_text_edit_count; i += 1) {
    RangeU64 range = {};
    if (!ResolveEditRange(buffer, item.additional_text_edits[i], ui->popup_encoding, &range)) {
      EditorSetStatus(ed, Str8Lit("LSP: invalid completion edit"));
      ClearPopup(ui);
      return false;
    }
    edits[i + 1] = PendingEdit{range, item.additional_text_edits[i].new_text};
  }

  for (u64 i = 0; i < edit_count; i += 1) {
    for (u64 j = i + 1; j < edit_count; j += 1) {
      if (RangesOverlap(edits[i].range, edits[j].range)) {
        EditorSetStatus(ed, Str8Lit("LSP: overlapping completion edits"));
        ClearPopup(ui);
        return false;
      }
    }
  }

  i64 cursor_shift = 0;
  for (u64 i = 1; i < edit_count; i += 1) {
    if (edits[i].range.max <= primary_range.min) {
      cursor_shift += (i64)edits[i].text.size;
      cursor_shift -= (i64)RangeLength(edits[i].range);
    }
  }
  i64 shifted_start = (i64)primary_range.min + cursor_shift;
  if (shifted_start < 0) shifted_start = 0;
  u64 cursor_after = (u64)shifted_start + primary_text.size;
  u64 cursor_before = view->cursor;

  std::sort(edits, edits + edit_count,
            [](const PendingEdit &a, const PendingEdit &b) { return a.range.min > b.range.min; });

  BufferBeginEditGroup(buffer);
  for (u64 i = 0; i < edit_count; i += 1) {
    BufferReplace(ed, buffer, edits[i].range, edits[i].text, cursor_before, cursor_after);
  }
  BufferEndEditGroup(buffer);

  view->cursor = Min(cursor_after, BufferSize(buffer));
  view->preferred_column = BufferColumnFromOffset(buffer, view->cursor);
  EditorScrollFocusedToCursor(ed);
  ClearPopup(ui);
  return true;
}

void CompletionResponse(void *user_data, const LspClientResponse *response) {
  EditorLspUi *ui = (EditorLspUi *)user_data;
  if (ui == nullptr || response == nullptr) return;
  if (ui->pending_completion.request_id == 0 || response->id != ui->pending_completion.request_id) return;

  PendingRequest pending = ui->pending_completion;
  ClearPending(&ui->pending_completion);

  Buffer *buffer = pending.editor ? BufferFromHandle(&pending.editor->buffers, pending.buffer) : nullptr;
  if (response->cancelled || response->has_error || !response->has_result || buffer == nullptr) {
    ClearPopup(ui);
    if (pending.editor != nullptr && response->has_error) {
      EditorSetStatusF(pending.editor, "LSP: completion failed: %.*s", (int)response->error_message.size,
                       (char *)response->error_message.str);
    }
    return;
  }

  String8 error = {};
  if (!EditorLspUiShowCompletionResult(ui, buffer, pending.encoding, pending.anchor_offset,
                                       pending.anchor_version, response->result, &error) &&
      pending.editor != nullptr) {
    EditorSetStatusF(pending.editor, "LSP: completion failed: %.*s", (int)error.size,
                     (char *)error.str);
  }
}

void HoverResponse(void *user_data, const LspClientResponse *response) {
  EditorLspUi *ui = (EditorLspUi *)user_data;
  if (ui == nullptr || response == nullptr) return;
  if (ui->pending_hover.request_id == 0 || response->id != ui->pending_hover.request_id) return;

  PendingRequest pending = ui->pending_hover;
  ClearPending(&ui->pending_hover);

  Buffer *buffer = pending.editor ? BufferFromHandle(&pending.editor->buffers, pending.buffer) : nullptr;
  if (response->cancelled || response->has_error || !response->has_result || buffer == nullptr) {
    ClearPopup(ui);
    if (pending.editor != nullptr && response->has_error) {
      EditorSetStatusF(pending.editor, "LSP: hover failed: %.*s", (int)response->error_message.size,
                       (char *)response->error_message.str);
    }
    return;
  }

  String8 error = {};
  if (!EditorLspUiShowHoverResult(ui, buffer, pending.encoding, pending.anchor_offset,
                                  pending.anchor_version, response->result, &error) &&
      pending.editor != nullptr) {
    EditorSetStatusF(pending.editor, "LSP: hover failed: %.*s", (int)error.size,
                     (char *)error.str);
  }
}

bool BuildTextDocumentPositionParams(Arena *arena, Buffer *buffer, u64 cursor_offset,
                                     LspPositionEncoding encoding, String8 *json) {
  if (arena == nullptr || buffer == nullptr || json == nullptr) return false;
  String8 uri = LspPathToUri(arena, buffer->path);
  if (uri.size == 0) return false;

  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);
  LspPosition position = LspPositionFromOffset(buffer, cursor_offset, encoding);
  bool ok = JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("textDocument")) && JsonWriteObjectBegin(&writer) &&
            JsonWriteObjectKey(&writer, Str8Lit("uri")) && JsonWriteString(&writer, uri) &&
            JsonWriteObjectEnd(&writer) && JsonWriteObjectKey(&writer, Str8Lit("position")) &&
            JsonWriteObjectBegin(&writer) && JsonWriteObjectKey(&writer, Str8Lit("line")) &&
            JsonWriteU64(&writer, position.line) &&
            JsonWriteObjectKey(&writer, Str8Lit("character")) &&
            JsonWriteU64(&writer, position.character) && JsonWriteObjectEnd(&writer) &&
            JsonWriteObjectEnd(&writer);
  *json = ok ? JsonWriterFinish(&writer) : String8{};
  JsonWriterDestroy(&writer);
  return ok && json->size > 0;
}

}  // namespace

EditorLspUi *EditorLspUiCreate() {
  EditorLspUi *ui = new (std::nothrow) EditorLspUi();
  if (ui == nullptr) return nullptr;
  ui->popup_arena = ArenaAlloc(kPopupArenaReserve);
  return ui;
}

void EditorLspUiDestroy(EditorLspUi *ui) {
  if (ui == nullptr) return;
  ResetDocuments(ui);
  if (ui->popup_arena != nullptr) ArenaRelease(ui->popup_arena);
  delete ui;
}

void EditorLspUiReset(EditorLspUi *ui) {
  if (ui == nullptr) return;
  ResetDocuments(ui);
  ClearPopup(ui);
  ClearPending(&ui->pending_completion);
  ClearPending(&ui->pending_hover);
}

void EditorLspUiOnBufferClosed(EditorLspUi *ui, Buffer *buffer) {
  if (ui == nullptr || buffer == nullptr) return;
  TempArena scratch = ScratchBegin();
  String8 canonical_uri = CanonicalUriForBuffer(scratch.arena, buffer);
  if (canonical_uri.size > 0) {
    for (u64 i = 0; i < ui->documents.size(); i += 1) {
      if (!Str8Match(ui->documents[i].uri, canonical_uri)) continue;
      ReleaseDocument(&ui->documents[i]);
      ui->documents.erase(
          ui->documents.begin() +
          static_cast<std::vector<EditorLspUiDocument>::difference_type>(i));
      break;
    }
  }
  if (BufferHandleEqual(ui->popup.anchor_buffer, buffer->handle)) ClearPopup(ui);
  if (BufferHandleEqual(ui->pending_completion.buffer, buffer->handle)) ClearPending(&ui->pending_completion);
  if (BufferHandleEqual(ui->pending_hover.buffer, buffer->handle)) ClearPending(&ui->pending_hover);
  ScratchEnd(scratch);
}

void EditorLspUiInvalidatePopup(EditorLspUi *ui) { ClearPopup(ui); }

void EditorLspUiInvalidatePopupIfStale(EditorLspUi *ui, Buffer *buffer) {
  if (ui == nullptr || ui->popup.kind == EditorLspUiPopupKind::None) return;
  if (buffer == nullptr || !BufferHandleEqual(buffer->handle, ui->popup.anchor_buffer) ||
      (i64)buffer->edit_serial != ui->popup.anchor_version) {
    ClearPopup(ui);
  }
}

const EditorLspUiPopupView *EditorLspUiPopup(const EditorLspUi *ui) {
  return ui ? &ui->popup : nullptr;
}

const LspDiagnostic *EditorLspUiDiagnosticsForBuffer(EditorLspUi *ui, Buffer *buffer, u64 *count) {
  if (count != nullptr) *count = 0;
  if (ui == nullptr || buffer == nullptr) return nullptr;
  TempArena scratch = ScratchBegin();
  String8 canonical_uri = CanonicalUriForBuffer(scratch.arena, buffer);
  EditorLspUiDocument *document = FindDocument(ui, canonical_uri);
  ScratchEnd(scratch);
  if (document == nullptr) return nullptr;
  if (count != nullptr) *count = document->diagnostic_count;
  return document->diagnostics;
}

const LspDiagnostic *EditorLspUiDiagnosticsForUri(EditorLspUi *ui, String8 uri, u64 *count) {
  if (count != nullptr) *count = 0;
  if (ui == nullptr || uri.size == 0) return nullptr;
  TempArena scratch = ScratchBegin();
  String8 canonical_uri = CanonicalUri(scratch.arena, uri);
  EditorLspUiDocument *document = FindDocument(ui, canonical_uri);
  ScratchEnd(scratch);
  if (document == nullptr) return nullptr;
  if (count != nullptr) *count = document->diagnostic_count;
  return document->diagnostics;
}

LspDiagnosticSeverity EditorLspUiHighestSeverityForLine(EditorLspUi *ui, Buffer *buffer, u64 line) {
  u64 count = 0;
  const LspDiagnostic *diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  u8 best = 0;
  for (u64 i = 0; i < count; i += 1) {
    const LspDiagnostic &diagnostic = diagnostics[i];
    u64 last_line = diagnostic.range.end.line;
    if (diagnostic.range.end.character == 0 && diagnostic.range.end.line > diagnostic.range.start.line) {
      last_line -= 1;
    }
    if (diagnostic.range.start.line <= line && line <= last_line) {
      if (best == 0 || (u8)diagnostic.severity < best) best = (u8)diagnostic.severity;
    }
  }
  return (LspDiagnosticSeverity)best;
}

bool EditorLspUiApplyPublishDiagnostics(EditorLspUi *ui, Editor *ed, const JsonValue *params,
                                        String8 *error) {
  if (ui == nullptr || ed == nullptr) return false;
  TempArena scratch = ScratchBegin();
  LspPublishDiagnostics payload = {};
  if (!LspDecodePublishDiagnostics(scratch.arena, params, &payload, error)) {
    ScratchEnd(scratch);
    return false;
  }

  String8 canonical_uri = CanonicalUri(scratch.arena, payload.uri);
  EditorLspUiDocument *existing = FindDocument(ui, canonical_uri);
  Buffer *buffer = FindBufferForUri(ed, canonical_uri);

  if (payload.has_version) {
    if (existing != nullptr && existing->has_version && payload.version < existing->version) {
      ScratchEnd(scratch);
      return true;
    }
    if (buffer != nullptr && payload.version < (i64)buffer->edit_serial) {
      ScratchEnd(scratch);
      return true;
    }
  }

  EditorLspUiDocument *document = existing != nullptr ? existing : EnsureDocument(ui, canonical_uri);
  if (document == nullptr) {
    ScratchEnd(scratch);
    return false;
  }

  LspPositionEncoding previous_encoding =
      existing != nullptr ? existing->encoding : LspPositionEncoding::Utf16;
  ReleaseDocument(document);
  document->arena = ArenaAlloc(kDocumentArenaReserve);
  document->uri = PushStr8Copy(document->arena, canonical_uri);
  document->has_version = payload.has_version;
  document->version = payload.version;
  document->encoding = InferEncoding(ed, buffer, previous_encoding);
  document->diagnostic_count = payload.diagnostic_count;
  if (document->diagnostic_count > 0) {
    document->diagnostics = PushArray(document->arena, LspDiagnostic, document->diagnostic_count);
    for (u64 i = 0; i < document->diagnostic_count; i += 1) {
      document->diagnostics[i] = {};
      document->diagnostics[i].range = payload.diagnostics[i].range;
      document->diagnostics[i].severity = payload.diagnostics[i].severity;
      document->diagnostics[i].code = PushStr8Copy(document->arena, payload.diagnostics[i].code);
      document->diagnostics[i].source = PushStr8Copy(document->arena, payload.diagnostics[i].source);
      document->diagnostics[i].message = PushStr8Copy(document->arena, payload.diagnostics[i].message);
    }
  }

  if (buffer != nullptr && ui->popup.kind == EditorLspUiPopupKind::Diagnostic &&
      BufferHandleEqual(ui->popup.anchor_buffer, buffer->handle)) {
    ClearPopup(ui);
  }

  ScratchEnd(scratch);
  return true;
}

u64 EditorLspUiRequestCompletion(EditorLspUi *ui, Editor *ed, Buffer *buffer, u64 cursor_offset) {
  if (ui == nullptr || ed == nullptr || buffer == nullptr) return 0;
  EditorLspBufferInfo info = {};
  if (!EditorLspGetBufferInfo(ed, buffer, &info) || !info.did_open_sent || info.capabilities == nullptr ||
      !info.capabilities->completion_provider) {
    EditorSetStatus(ed, Str8Lit("LSP: completion unavailable"));
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params_json = {};
  if (!BuildTextDocumentPositionParams(scratch.arena, buffer, cursor_offset, info.position_encoding,
                                       &params_json)) {
    EditorSetStatus(ed, Str8Lit("LSP: completion unavailable"));
    ScratchEnd(scratch);
    return 0;
  }

  ClearPending(&ui->pending_completion);
  ui->pending_completion.editor = ed;
  ui->pending_completion.buffer = buffer->handle;
  ui->pending_completion.anchor_offset = cursor_offset;
  ui->pending_completion.anchor_version = (i64)buffer->edit_serial;
  ui->pending_completion.encoding = info.position_encoding;
  ui->pending_completion.request_id =
      EditorLspSendRequestJson(ed, buffer, Str8Lit("textDocument/completion"), params_json,
                               kCompletionRequestKind, nullptr, CompletionResponse, ui);
  if (ui->pending_completion.request_id == 0) {
    EditorLspApplyClientFailureStatus(ed, buffer);
    ClearPending(&ui->pending_completion);
  }
  ScratchEnd(scratch);
  return ui->pending_completion.request_id;
}

bool EditorLspUiShowCompletionResult(EditorLspUi *ui, Buffer *buffer, LspPositionEncoding encoding,
                                     u64 anchor_offset, i64 anchor_version, const JsonValue *result,
                                     String8 *error) {
  if (ui == nullptr || buffer == nullptr) return false;
  if ((i64)buffer->edit_serial != anchor_version) {
    ClearPopup(ui);
    return true;
  }

  TempArena scratch = ScratchBegin();
  LspCompletionResult completion = {};
  if (!LspDecodeCompletionResult(scratch.arena, result, &completion, error)) {
    ScratchEnd(scratch);
    return false;
  }
  if (!completion.present || completion.count == 0) {
    ClearPopup(ui);
    ScratchEnd(scratch);
    return true;
  }

  ClearPopup(ui);
  ui->popup.kind = EditorLspUiPopupKind::Completion;
  ui->popup.anchor_buffer = buffer->handle;
  ui->popup.anchor_offset = anchor_offset;
  ui->popup.anchor_version = anchor_version;
  ui->popup_encoding = encoding;
  ui->popup.completion.count = Min(completion.count, kMaxCompletionItems);
  ui->popup.completion.items =
      PushArray(ui->popup_arena, EditorLspUiCompletionItem, ui->popup.completion.count);
  for (u64 i = 0; i < ui->popup.completion.count; i += 1) {
    CopyCompletionItem(ui->popup_arena, completion.items[i],
                       (EditorLspUiCompletionItem *)&ui->popup.completion.items[i]);
  }
  MaybeSortCompletionItems((EditorLspUiCompletionItem *)ui->popup.completion.items,
                           ui->popup.completion.count);
  ScratchEnd(scratch);
  return true;
}

u64 EditorLspUiRequestHover(EditorLspUi *ui, Editor *ed, Buffer *buffer, u64 cursor_offset) {
  if (ui == nullptr || ed == nullptr || buffer == nullptr) return 0;
  EditorLspBufferInfo info = {};
  if (!EditorLspGetBufferInfo(ed, buffer, &info) || !info.did_open_sent || info.capabilities == nullptr ||
      !info.capabilities->hover_provider) {
    EditorSetStatus(ed, Str8Lit("LSP: hover unavailable"));
    return 0;
  }

  TempArena scratch = ScratchBegin();
  String8 params_json = {};
  if (!BuildTextDocumentPositionParams(scratch.arena, buffer, cursor_offset, info.position_encoding,
                                       &params_json)) {
    EditorSetStatus(ed, Str8Lit("LSP: hover unavailable"));
    ScratchEnd(scratch);
    return 0;
  }

  ClearPending(&ui->pending_hover);
  ui->pending_hover.editor = ed;
  ui->pending_hover.buffer = buffer->handle;
  ui->pending_hover.anchor_offset = cursor_offset;
  ui->pending_hover.anchor_version = (i64)buffer->edit_serial;
  ui->pending_hover.encoding = info.position_encoding;
  ui->pending_hover.request_id = EditorLspSendRequestJson(ed, buffer, Str8Lit("textDocument/hover"),
                                                          params_json, kHoverRequestKind, nullptr,
                                                          HoverResponse, ui);
  if (ui->pending_hover.request_id == 0) {
    EditorLspApplyClientFailureStatus(ed, buffer);
    ClearPending(&ui->pending_hover);
  }
  ScratchEnd(scratch);
  return ui->pending_hover.request_id;
}

bool EditorLspUiShowHoverResult(EditorLspUi *ui, Buffer *buffer, LspPositionEncoding encoding,
                                u64 anchor_offset, i64 anchor_version, const JsonValue *result,
                                String8 *error) {
  (void)encoding;
  if (ui == nullptr || buffer == nullptr) return false;
  if ((i64)buffer->edit_serial != anchor_version) {
    ClearPopup(ui);
    return true;
  }

  TempArena scratch = ScratchBegin();
  LspHover hover = {};
  if (!LspDecodeHover(scratch.arena, result, &hover, error)) {
    ScratchEnd(scratch);
    return false;
  }
  if (!hover.present || hover.text.size == 0) {
    ClearPopup(ui);
    ScratchEnd(scratch);
    return true;
  }

  bool ok =
      SetPopupTextFromString(ui, EditorLspUiPopupKind::Hover, buffer, anchor_offset, anchor_version,
                             hover.has_range, hover.range, hover.text);
  ScratchEnd(scratch);
  return ok;
}

bool EditorLspUiBuildDiagnosticPopup(EditorLspUi *ui, Buffer *buffer, u64 cursor_offset,
                                     String8 *status) {
  if (status != nullptr) *status = {};
  if (ui == nullptr || buffer == nullptr) return false;

  TempArena scratch = ScratchBegin();
  String8 canonical_uri = CanonicalUriForBuffer(scratch.arena, buffer);
  EditorLspUiDocument *document = FindDocument(ui, canonical_uri);
  if (document == nullptr || document->diagnostic_count == 0) {
    ClearPopup(ui);
    if (status != nullptr) *status = Str8Lit("No diagnostics at cursor");
    ScratchEnd(scratch);
    return false;
  }

  LspPosition cursor = LspPositionFromOffset(buffer, cursor_offset, document->encoding);
  String8 *lines = PushArray(scratch.arena, String8, document->diagnostic_count);
  u64 count = 0;
  for (u64 i = 0; i < document->diagnostic_count; i += 1) {
    const LspDiagnostic &diagnostic = document->diagnostics[i];
    if (!RangeContainsPosition(diagnostic.range, cursor)) continue;

    if (diagnostic.source.size > 0 && diagnostic.code.size > 0) {
      lines[count++] = PushStr8F(scratch.arena, "%s: %.*s (%.*s %.*s)",
                                 SeverityName(diagnostic.severity), (int)diagnostic.message.size,
                                 (char *)diagnostic.message.str, (int)diagnostic.source.size,
                                 (char *)diagnostic.source.str, (int)diagnostic.code.size,
                                 (char *)diagnostic.code.str);
    } else if (diagnostic.source.size > 0) {
      lines[count++] = PushStr8F(scratch.arena, "%s: %.*s (%.*s)",
                                 SeverityName(diagnostic.severity), (int)diagnostic.message.size,
                                 (char *)diagnostic.message.str, (int)diagnostic.source.size,
                                 (char *)diagnostic.source.str);
    } else if (diagnostic.code.size > 0) {
      lines[count++] = PushStr8F(scratch.arena, "%s: %.*s (%.*s)",
                                 SeverityName(diagnostic.severity), (int)diagnostic.message.size,
                                 (char *)diagnostic.message.str, (int)diagnostic.code.size,
                                 (char *)diagnostic.code.str);
    } else {
      lines[count++] = PushStr8F(scratch.arena, "%s: %.*s", SeverityName(diagnostic.severity),
                                 (int)diagnostic.message.size, (char *)diagnostic.message.str);
    }
  }

  if (count == 0) {
    ClearPopup(ui);
    if (status != nullptr) *status = Str8Lit("No diagnostics at cursor");
    ScratchEnd(scratch);
    return false;
  }

  bool ok = SetPopupText(ui, EditorLspUiPopupKind::Diagnostic, buffer, cursor_offset,
                         (i64)buffer->edit_serial, false, {}, lines, count);
  ScratchEnd(scratch);
  return ok;
}

bool EditorLspUiHandlePopupInput(EditorLspUi *ui, Editor *ed, View *view, KeyChord chord) {
  if (ui == nullptr || ui->popup.kind == EditorLspUiPopupKind::None) return false;

  if (KeyChordEqual(chord, KeyChordKey(Key::Escape))) {
    ClearPopup(ui);
    return true;
  }

  if (ui->popup.kind == EditorLspUiPopupKind::Completion) {
    u64 count = ui->popup.completion.count;
    if (count == 0) {
      ClearPopup(ui);
      return false;
    }

    if (KeyChordEqual(chord, KeyChordKey(Key::Up)) ||
        KeyChordEqual(chord, KeyChordKey(Key::P, KeyMod::Ctrl))) {
      ui->popup.completion.selected = (ui->popup.completion.selected + count - 1) % count;
      ui->popup.completion.scroll = ui->popup.completion.selected;
      return true;
    }
    if (KeyChordEqual(chord, KeyChordKey(Key::Down)) ||
        KeyChordEqual(chord, KeyChordKey(Key::N, KeyMod::Ctrl))) {
      ui->popup.completion.selected = (ui->popup.completion.selected + 1) % count;
      ui->popup.completion.scroll = ui->popup.completion.selected;
      return true;
    }
    if (KeyChordEqual(chord, KeyChordKey(Key::Return))) {
      (void)CompletionAccept(ui, ed, view);
      return true;
    }
  }

  ClearPopup(ui);
  return false;
}
