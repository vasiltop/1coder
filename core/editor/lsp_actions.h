#pragma once

#include "editor/buffer.h"
#include "lsp/client.h"
#include "lsp/protocol.h"

struct Editor;

enum class EditorLspNavigationKind : u8 {
  Declaration,
  Definition,
  Implementation,
  TypeDefinition,
};

enum class EditorLspRenamePrepareStatus : u8 { Ready, Rejected, Unsupported, Failed };

struct EditorLspRenamePrepareResult {
  EditorLspRenamePrepareStatus status;
  RangeU64 range;
  String8 placeholder;
  String8 prompt;
};

using EditorLspRenamePrepareProc = void (*)(void *user_data,
                                            const EditorLspRenamePrepareResult *result);

struct EditorLspFormatContext {
  Editor *ed;
  BufferHandle buffer;
  String8 uri;
  LspPositionEncoding position_encoding;
  bool owned;
};

struct EditorLspNavigationContext {
  Editor *ed;
  BufferHandle buffer;
  String8 uri;
  EditorLspNavigationKind kind;
  LspPositionEncoding position_encoding;
  bool owned;
};

struct EditorLspRenamePrepareContext {
  Editor *ed;
  BufferHandle buffer;
  String8 uri;
  EditorLspRenamePrepareProc callback;
  void *callback_user_data;
  LspPositionEncoding position_encoding;
  bool owned;
};

struct EditorLspRenameContext {
  Editor *ed;
  BufferHandle buffer;
  String8 uri;
  LspPositionEncoding position_encoding;
  bool owned;
};

[[nodiscard]] u64 EditorLspRequestFormatting(Editor *ed, Buffer *buffer);
[[nodiscard]] u64 EditorLspRequestNavigation(Editor *ed, Buffer *buffer,
                                             EditorLspNavigationKind kind);
[[nodiscard]] u64 EditorLspPrepareRename(Editor *ed, Buffer *buffer,
                                         EditorLspRenamePrepareProc callback,
                                         void *callback_user_data);
[[nodiscard]] u64 EditorLspSubmitRename(Editor *ed, Buffer *buffer, String8 new_name);

void EditorLspOnFormattingResponse(void *user_data, const LspClientResponse *response);
void EditorLspOnNavigationResponse(void *user_data, const LspClientResponse *response);
void EditorLspOnPrepareRenameResponse(void *user_data, const LspClientResponse *response);
void EditorLspOnRenameResponse(void *user_data, const LspClientResponse *response);

[[nodiscard]] bool EditorLspApplyTextEdits(Editor *ed, Buffer *buffer, i64 expected_version,
                                           LspPositionEncoding position_encoding,
                                           const LspTextEdit *edits, u64 edit_count);
[[nodiscard]] bool EditorLspApplyWorkspaceEdit(Editor *ed, const LspWorkspaceEdit *edit,
                                               LspPositionEncoding position_encoding);
