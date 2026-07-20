#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

struct Buffer;
struct JsonValue;

enum class LspPositionEncoding : u8 { Utf8, Utf16, Utf32 };
enum class LspDiagnosticSeverity : u8 { Error = 1, Warning = 2, Information = 3, Hint = 4 };
enum class LspTextDocumentSyncKind : u8 { None = 0, Full = 1, Incremental = 2 };
enum class LspInsertTextFormat : u8 { PlainText = 1, Snippet = 2 };
enum class LspCompletionTextEditKind : u8 { None, TextEdit, InsertReplace };
enum class LspWorkspaceEditEntryKind : u8 { TextDocumentEdits, CreateFile, RenameFile, DeleteFile };

struct LspPosition {
  u32 line;
  u32 character;
};

struct LspRange {
  LspPosition start;
  LspPosition end;
};

struct LspLocation {
  String8 uri;
  LspRange range;
};

struct LspTextEdit {
  LspRange range;
  String8 new_text;
};

struct LspLocationLink {
  bool has_origin_selection_range;
  LspRange origin_selection_range;
  String8 target_uri;
  LspRange target_range;
  bool has_target_selection_range;
  LspRange target_selection_range;
};

struct LspTextDocumentSync {
  LspTextDocumentSyncKind kind;
  bool open_close;
  bool save;
  bool save_include_text;
};

struct LspWorkspaceFoldersCapabilities {
  bool supported;
  bool change_notifications;
  String8 change_notifications_id;
};

struct LspWorkspaceEditCapabilities {
  bool document_changes;
  String8 *resource_operations;
  u64 resource_operation_count;
};

struct LspServerCapabilities {
  LspPositionEncoding position_encoding;
  LspTextDocumentSync text_document_sync;
  bool completion_provider;
  bool hover_provider;
  bool declaration_provider;
  bool definition_provider;
  bool implementation_provider;
  bool type_definition_provider;
  bool document_formatting_provider;
  bool rename_provider;
  bool rename_prepare_provider;
  LspWorkspaceFoldersCapabilities workspace_folders;
  LspWorkspaceEditCapabilities workspace_edit;
};

struct LspInitializeResult {
  LspServerCapabilities capabilities;
};

struct LspInsertReplaceEdit {
  LspRange insert;
  LspRange replace;
  String8 new_text;
};

struct LspCompletionItem {
  String8 label;
  String8 detail;
  String8 filter_text;
  String8 sort_text;
  String8 insert_text;
  LspInsertTextFormat insert_text_format;
  LspCompletionTextEditKind text_edit_kind;
  LspTextEdit text_edit;
  LspInsertReplaceEdit insert_replace_edit;
  LspTextEdit *additional_text_edits;
  u64 additional_text_edit_count;
};

struct LspCompletionResult {
  bool present;
  bool is_incomplete;
  LspCompletionItem *items;
  u64 count;
};

struct LspHover {
  bool present;
  String8 text;
  String8 language;
  String8 kind;
  bool has_range;
  LspRange range;
};

struct LspDiagnosticRelatedInformation {
  LspLocation location;
  String8 message;
};

struct LspDiagnostic {
  LspRange range;
  LspDiagnosticSeverity severity;
  String8 code;
  String8 source;
  String8 message;
  LspDiagnosticRelatedInformation *related_information;
  u64 related_information_count;
  u64 *tags;
  u64 tag_count;
};

struct LspPublishDiagnostics {
  String8 uri;
  bool has_version;
  i64 version;
  LspDiagnostic *diagnostics;
  u64 diagnostic_count;
};

struct LspWorkspaceEditEntry {
  LspWorkspaceEditEntryKind kind;
  String8 uri;
  String8 old_uri;
  String8 new_uri;
  bool has_version;
  i64 version;
  LspTextEdit *edits;
  u64 edit_count;
};

struct LspWorkspaceEdit {
  bool present;
  LspWorkspaceEditEntry *entries;
  u64 count;
};

[[nodiscard]] String8 LspPathToUri(Arena *arena, String8 absolute_path);
// Returns an empty String8 on invalid or unsupported URIs.
[[nodiscard]] String8 LspUriToPath(Arena *arena, String8 uri);
[[nodiscard]] LspPosition LspPositionFromOffset(const Buffer *buffer, u64 offset,
                                                LspPositionEncoding encoding);
[[nodiscard]] u64 LspOffsetFromPosition(const Buffer *buffer, LspPosition position,
                                        LspPositionEncoding encoding);

[[nodiscard]] bool LspDecodeRange(Arena *arena, const JsonValue *value, LspRange *range,
                                  String8 *error);
[[nodiscard]] bool LspDecodeLocation(Arena *arena, const JsonValue *value, LspLocation *location,
                                     String8 *error);
[[nodiscard]] bool LspDecodeLocationLink(Arena *arena, const JsonValue *value,
                                         LspLocationLink *location_link, String8 *error);
[[nodiscard]] bool LspDecodeLocationResult(Arena *arena, const JsonValue *value,
                                           LspLocation **locations, u64 *count, String8 *error);
[[nodiscard]] bool LspDecodeInitializeResult(Arena *arena, const JsonValue *value,
                                             LspInitializeResult *result, String8 *error);
[[nodiscard]] bool LspDecodeCompletionResult(Arena *arena, const JsonValue *value,
                                             LspCompletionResult *result, String8 *error);
[[nodiscard]] bool LspDecodeHover(Arena *arena, const JsonValue *value, LspHover *hover,
                                  String8 *error);
[[nodiscard]] bool LspDecodePublishDiagnostics(Arena *arena, const JsonValue *value,
                                               LspPublishDiagnostics *payload, String8 *error);
[[nodiscard]] bool LspDecodeWorkspaceEdit(Arena *arena, const JsonValue *value,
                                          LspWorkspaceEdit *edit, String8 *error);
[[nodiscard]] bool LspDecodeTextEdits(Arena *arena, const JsonValue *value, LspTextEdit **edits,
                                      u64 *count, String8 *error);
