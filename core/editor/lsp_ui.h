#pragma once

#include "base/base_string.h"
#include "base/base_types.h"
#include "editor/buffer.h"
#include "input/keys.h"
#include "lsp/protocol.h"

struct Editor;
struct JsonValue;
struct View;

struct EditorLspUi;

enum class EditorLspUiPopupKind : u8 { None = 0, Completion, Hover, Diagnostic };

struct EditorLspUiCompletionItem {
  String8 label;
  String8 detail;
  String8 filter_text;
  String8 sort_text;
  String8 insert_text;
  LspCompletionTextEditKind text_edit_kind;
  LspTextEdit text_edit;
  LspInsertReplaceEdit insert_replace_edit;
  LspTextEdit *additional_text_edits;
  u64 additional_text_edit_count;
};

struct EditorLspUiCompletionPopup {
  const EditorLspUiCompletionItem *items;
  u64 count;
  u64 selected;
  u64 scroll;
};

struct EditorLspUiTextPopup {
  const String8 *lines;
  u64 line_count;
  bool has_range;
  LspRange range;
};

struct EditorLspUiPopupView {
  EditorLspUiPopupKind kind;
  BufferHandle anchor_buffer;
  u64 anchor_offset;
  i64 anchor_version;
  EditorLspUiCompletionPopup completion;
  EditorLspUiTextPopup text;
};

EditorLspUi *EditorLspUiCreate();
void EditorLspUiDestroy(EditorLspUi *ui);
void EditorLspUiReset(EditorLspUi *ui);

void EditorLspUiOnBufferClosed(EditorLspUi *ui, Buffer *buffer);
void EditorLspUiInvalidatePopup(EditorLspUi *ui);
void EditorLspUiInvalidatePopupIfStale(EditorLspUi *ui, Buffer *buffer);

[[nodiscard]] const EditorLspUiPopupView *EditorLspUiPopup(const EditorLspUi *ui);

[[nodiscard]] const LspDiagnostic *EditorLspUiDiagnosticsForBuffer(EditorLspUi *ui, Buffer *buffer,
                                                                   u64 *count);
[[nodiscard]] const LspDiagnostic *EditorLspUiDiagnosticsForUri(EditorLspUi *ui, String8 uri,
                                                                u64 *count);
[[nodiscard]] LspDiagnosticSeverity EditorLspUiHighestSeverityForLine(EditorLspUi *ui,
                                                                      Buffer *buffer, u64 line);

[[nodiscard]] bool EditorLspUiApplyPublishDiagnostics(EditorLspUi *ui, Editor *ed,
                                                      const JsonValue *params, String8 *error);

[[nodiscard]] u64 EditorLspUiRequestCompletion(EditorLspUi *ui, Editor *ed, Buffer *buffer,
                                               u64 cursor_offset);
[[nodiscard]] bool EditorLspUiShowCompletionResult(EditorLspUi *ui, Buffer *buffer,
                                                   LspPositionEncoding encoding,
                                                   u64 anchor_offset, i64 anchor_version,
                                                   const JsonValue *result, String8 *error);

[[nodiscard]] u64 EditorLspUiRequestHover(EditorLspUi *ui, Editor *ed, Buffer *buffer,
                                          u64 cursor_offset);
[[nodiscard]] bool EditorLspUiShowHoverResult(EditorLspUi *ui, Buffer *buffer,
                                              LspPositionEncoding encoding, u64 anchor_offset,
                                              i64 anchor_version, const JsonValue *result,
                                              String8 *error);

[[nodiscard]] bool EditorLspUiBuildDiagnosticPopup(EditorLspUi *ui, Buffer *buffer,
                                                   u64 cursor_offset, String8 *status);

[[nodiscard]] bool EditorLspUiHandlePopupInput(EditorLspUi *ui, Editor *ed, View *view,
                                               KeyChord chord);
