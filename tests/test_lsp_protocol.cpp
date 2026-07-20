#include "editor/buffer.h"
#include "lsp/json.h"
#include "lsp/protocol.h"
#include "test.h"

namespace {

struct ArenaScope {
  Arena *arena;

  explicit ArenaScope(u64 reserve_size = MB(32)) : arena(ArenaAlloc(reserve_size)) {}
  ~ArenaScope() { ArenaRelease(arena); }
};

struct BufferScope {
  Buffer buffer;

  explicit BufferScope(String8 text) {
    BufferInit(&buffer, BufferKind::Scratch, Str8Lit("lsp"));
    BufferSetText(nullptr, &buffer, text);
  }

  ~BufferScope() { BufferDestroy(&buffer); }
};

JsonValue *ParseJsonOrFail(Arena *arena, String8 text) {
  JsonParseResult result = JsonParse(arena, text);
  if (result.root == nullptr) {
    TestFail(__FILE__, __LINE__, "parse failed at %llu: %.*s",
             (unsigned long long)result.error_offset, (int)result.error.size,
             (char *)result.error.str);
  }
  return result.root;
}

void CheckRange(LspRange actual, u32 start_line, u32 start_char, u32 end_line, u32 end_char) {
  CHECK_EQ(actual.start.line, start_line);
  CHECK_EQ(actual.start.character, start_char);
  CHECK_EQ(actual.end.line, end_line);
  CHECK_EQ(actual.end.character, end_char);
}

void CheckDecodeFailure(bool ok, String8 error) {
  CHECK(!ok);
  CHECK(error.size > 0);
}

}  // namespace

TEST(lsp_protocol_file_uri_round_trips_paths) {
  ArenaScope scope;

  String8 posix_path = Str8Lit("/tmp/space #percent%?snowman-\xE2\x98\x83.txt");
  String8 posix_uri = LspPathToUri(scope.arena, posix_path);
  CHECK_STR(posix_uri,
            Str8Lit("file:///tmp/space%20%23percent%25%3Fsnowman-%E2%98%83.txt"));
  CHECK_STR(LspUriToPath(scope.arena, posix_uri), posix_path);

  String8 win_path = Str8Lit("C:\\Program Files\\A#B%20\\snowman-\xE2\x98\x83?.txt");
  String8 win_uri = LspPathToUri(scope.arena, win_path);
  CHECK_STR(
      win_uri,
      Str8Lit("file:///C:/Program%20Files/A%23B%2520/snowman-%E2%98%83%3F.txt"));
  CHECK_STR(LspUriToPath(scope.arena, win_uri),
            Str8Lit("C:/Program Files/A#B%20/snowman-\xE2\x98\x83?.txt"));

  String8 unc_path = Str8Lit("//server/share/My File.txt");
  String8 unc_uri = LspPathToUri(scope.arena, unc_path);
  CHECK_STR(unc_uri, Str8Lit("file://server/share/My%20File.txt"));
  CHECK_STR(LspUriToPath(scope.arena, unc_uri), unc_path);
}

TEST(lsp_protocol_file_uri_rejects_invalid_inputs) {
  ArenaScope scope;

  CHECK_STR(LspUriToPath(scope.arena, Str8Lit("file:///tmp/Case%2d%2E%7E.txt")),
            Str8Lit("/tmp/Case-.~.txt"));
  CHECK(LspUriToPath(scope.arena, Str8Lit("https://example.com/file")).size == 0);
  CHECK(LspUriToPath(scope.arena, Str8Lit("file:relative.txt")).size == 0);
  CHECK(LspUriToPath(scope.arena, Str8Lit("file:///tmp/bad%2")).size == 0);
  CHECK(LspUriToPath(scope.arena, Str8Lit("file:///tmp/bad%XZ")).size == 0);

  u8 with_nul[] = {'f','i','l','e',':','/','/','/','t','m','p','/','a',0,'b'};
  CHECK(LspUriToPath(scope.arena, Str8(with_nul, ArrayCount(with_nul))).size == 0);
}

TEST(lsp_protocol_position_converts_across_encodings) {
  BufferScope buffer_scope(Str8Lit("a\xC3\xA9\xF0\x9D\x84\x9E\n\xCE\xB2\xF0\x9F\x99\x82z"));
  Buffer *buffer = &buffer_scope.buffer;

  LspPosition utf8_mid = LspPositionFromOffset(buffer, 5, LspPositionEncoding::Utf8);
  CHECK_EQ(utf8_mid.line, 0);
  CHECK_EQ(utf8_mid.character, 3);

  LspPosition utf16_end = LspPositionFromOffset(buffer, 15, LspPositionEncoding::Utf16);
  CHECK_EQ(utf16_end.line, 1);
  CHECK_EQ(utf16_end.character, 4);

  LspPosition utf32_end = LspPositionFromOffset(buffer, 999, LspPositionEncoding::Utf32);
  CHECK_EQ(utf32_end.line, 1);
  CHECK_EQ(utf32_end.character, 3);

  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 0}, LspPositionEncoding::Utf8), (u64)0);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 2}, LspPositionEncoding::Utf8), (u64)1);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 3}, LspPositionEncoding::Utf8), (u64)3);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 7}, LspPositionEncoding::Utf8), (u64)7);

  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 3}, LspPositionEncoding::Utf16), (u64)3);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 4}, LspPositionEncoding::Utf16), (u64)7);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{1, 2}, LspPositionEncoding::Utf16), (u64)10);

  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{0, 99}, LspPositionEncoding::Utf32), (u64)7);
  CHECK_EQ(LspOffsetFromPosition(buffer, LspPosition{99, 0}, LspPositionEncoding::Utf8), (u64)15);
}

TEST(lsp_protocol_decodes_ranges_locations_and_location_results) {
  ArenaScope scope;

  JsonValue *range_json = ParseJsonOrFail(
      scope.arena, Str8Lit("{\"start\":{\"line\":1,\"character\":2},\"end\":{\"line\":3,\"character\":4}}"));
  LspRange range = {};
  String8 error = {};
  CHECK(LspDecodeRange(scope.arena, range_json, &range, &error));
  CheckRange(range, 1, 2, 3, 4);

  JsonValue *location_json = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"range\":{\"start\":{\"line\":5,\"character\":6},"
              "\"end\":{\"line\":5,\"character\":9}}}"));
  LspLocation location = {};
  CHECK(LspDecodeLocation(scope.arena, location_json, &location, &error));
  CHECK_STR(location.uri, Str8Lit("file:///tmp/a.cpp"));
  CheckRange(location.range, 5, 6, 5, 9);

  JsonValue *link_json = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"targetUri\":\"file:///tmp/b.cpp\","
              "\"targetRange\":{\"start\":{\"line\":1,\"character\":1},\"end\":{\"line\":1,\"character\":5}},"
              "\"targetSelectionRange\":{\"start\":{\"line\":2,\"character\":3},"
              "\"end\":{\"line\":2,\"character\":7}}}"));
  LspLocationLink link = {};
  CHECK(LspDecodeLocationLink(scope.arena, link_json, &link, &error));
  CHECK_STR(link.target_uri, Str8Lit("file:///tmp/b.cpp"));
  CheckRange(link.target_selection_range, 2, 3, 2, 7);

  JsonValue *mixed = ParseJsonOrFail(
      scope.arena,
      Str8Lit("[{\"uri\":\"file:///tmp/a.cpp\",\"range\":{\"start\":{\"line\":0,\"character\":1},"
              "\"end\":{\"line\":0,\"character\":4}}},"
              "{\"targetUri\":\"file:///tmp/b.cpp\","
              "\"targetRange\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":2}},"
              "\"targetSelectionRange\":{\"start\":{\"line\":7,\"character\":8},"
              "\"end\":{\"line\":7,\"character\":9}}},"
              "{\"targetUri\":\"file:///tmp/c.cpp\","
              "\"targetRange\":{\"start\":{\"line\":9,\"character\":1},\"end\":{\"line\":9,\"character\":2}}}]"));
  LspLocation *locations = nullptr;
  u64 count = 0;
  CHECK(LspDecodeLocationResult(scope.arena, mixed, &locations, &count, &error));
  CHECK_EQ(count, (u64)3);
  CHECK_STR(locations[1].uri, Str8Lit("file:///tmp/b.cpp"));
  CheckRange(locations[1].range, 7, 8, 7, 9);
  CheckRange(locations[2].range, 9, 1, 9, 2);

  locations = (LspLocation *)1;
  count = 99;
  CHECK(LspDecodeLocationResult(scope.arena, ParseJsonOrFail(scope.arena, Str8Lit("null")),
                                &locations, &count, &error));
  CHECK(locations == nullptr);
  CHECK_EQ(count, (u64)0);

  error = {};
  bool ok = LspDecodeLocationResult(
      scope.arena,
      ParseJsonOrFail(scope.arena, Str8Lit("[{\"uri\":\"file:///tmp/x.cpp\"},{\"uri\":\"file:///tmp/y.cpp\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}}}]")),
      &locations, &count, &error);
  CheckDecodeFailure(ok, error);
}

TEST(lsp_protocol_decodes_initialize_result_capabilities) {
  ArenaScope scope;
  String8 error = {};

  JsonValue *root = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"capabilities\":{"
              "\"positionEncoding\":\"utf-8\","
              "\"textDocumentSync\":{\"openClose\":true,\"change\":2,\"save\":{\"includeText\":true}},"
              "\"completionProvider\":{},"
              "\"hoverProvider\":true,"
              "\"declarationProvider\":{\"workDoneProgress\":true},"
              "\"definitionProvider\":true,"
              "\"implementationProvider\":false,"
              "\"typeDefinitionProvider\":{\"id\":1},"
              "\"documentFormattingProvider\":true,"
              "\"renameProvider\":{\"prepareProvider\":true},"
              "\"workspace\":{\"workspaceFolders\":{\"supported\":true,\"changeNotifications\":\"token\"},"
              "\"workspaceEdit\":{\"documentChanges\":true,\"resourceOperations\":[\"create\",\"rename\",\"delete\"]}}}}"));
  LspInitializeResult result = {};
  CHECK(LspDecodeInitializeResult(scope.arena, root, &result, &error));
  CHECK_EQ((u64)result.capabilities.position_encoding, (u64)LspPositionEncoding::Utf8);
  CHECK_EQ((u64)result.capabilities.text_document_sync.kind,
           (u64)LspTextDocumentSyncKind::Incremental);
  CHECK(result.capabilities.text_document_sync.open_close);
  CHECK(result.capabilities.text_document_sync.save);
  CHECK(result.capabilities.text_document_sync.save_include_text);
  CHECK(result.capabilities.completion_provider);
  CHECK(result.capabilities.hover_provider);
  CHECK(result.capabilities.declaration_provider);
  CHECK(result.capabilities.definition_provider);
  CHECK(!result.capabilities.implementation_provider);
  CHECK(result.capabilities.type_definition_provider);
  CHECK(result.capabilities.document_formatting_provider);
  CHECK(result.capabilities.rename_provider);
  CHECK(result.capabilities.rename_prepare_provider);
  CHECK(result.capabilities.workspace_folders.supported);
  CHECK(result.capabilities.workspace_folders.change_notifications);
  CHECK_STR(result.capabilities.workspace_folders.change_notifications_id, Str8Lit("token"));
  CHECK(result.capabilities.workspace_edit.document_changes);
  CHECK_EQ(result.capabilities.workspace_edit.resource_operation_count, (u64)3);

  root = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"capabilities\":{\"positionEncoding\":\"utf-19\",\"textDocumentSync\":1,"
              "\"hoverProvider\":{\"workDoneProgress\":true},\"renameProvider\":true}}"));
  result = {};
  CHECK(LspDecodeInitializeResult(scope.arena, root, &result, &error));
  CHECK_EQ((u64)result.capabilities.position_encoding, (u64)LspPositionEncoding::Utf16);
  CHECK_EQ((u64)result.capabilities.text_document_sync.kind, (u64)LspTextDocumentSyncKind::Full);
  CHECK(result.capabilities.hover_provider);
  CHECK(result.capabilities.rename_provider);
  CHECK(!result.capabilities.rename_prepare_provider);
}

TEST(lsp_protocol_decodes_completion_results_and_rejects_malformed_items) {
  ArenaScope scope;
  String8 error = {};

  LspCompletionResult null_result = {.present = true};
  CHECK(LspDecodeCompletionResult(scope.arena, ParseJsonOrFail(scope.arena, Str8Lit("null")),
                                  &null_result, &error));
  CHECK(!null_result.present);
  CHECK_EQ(null_result.count, (u64)0);

  JsonValue *array_json = ParseJsonOrFail(
      scope.arena,
      Str8Lit("[{\"label\":\"printf\",\"detail\":\"fn\",\"filterText\":\"printf\",\"sortText\":\"001\","
              "\"insertText\":\"printf($1)\",\"insertTextFormat\":2,"
              "\"textEdit\":{\"range\":{\"start\":{\"line\":1,\"character\":2},"
              "\"end\":{\"line\":1,\"character\":5}},\"newText\":\"printf\"},"
              "\"additionalTextEdits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":0}},\"newText\":\"#include <stdio.h>\\n\"}]}]"));
  LspCompletionResult result = {};
  CHECK(LspDecodeCompletionResult(scope.arena, array_json, &result, &error));
  CHECK(result.present);
  CHECK(!result.is_incomplete);
  CHECK_EQ(result.count, (u64)1);
  CHECK_STR(result.items[0].label, Str8Lit("printf"));
  CHECK_STR(result.items[0].detail, Str8Lit("fn"));
  CHECK_EQ((u64)result.items[0].insert_text_format, (u64)LspInsertTextFormat::Snippet);
  CHECK_EQ((u64)result.items[0].text_edit_kind, (u64)LspCompletionTextEditKind::TextEdit);
  CheckRange(result.items[0].text_edit.range, 1, 2, 1, 5);
  CHECK_EQ(result.items[0].additional_text_edit_count, (u64)1);

  JsonValue *list_json = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"isIncomplete\":true,\"items\":[{\"label\":\"x\","
              "\"textEdit\":{\"insert\":{\"start\":{\"line\":2,\"character\":1},"
              "\"end\":{\"line\":2,\"character\":1}},"
              "\"replace\":{\"start\":{\"line\":2,\"character\":1},"
              "\"end\":{\"line\":2,\"character\":4}},\"newText\":\"xyz\"}}]}"));
  result = {};
  CHECK(LspDecodeCompletionResult(scope.arena, list_json, &result, &error));
  CHECK(result.present);
  CHECK(result.is_incomplete);
  CHECK_EQ(result.count, (u64)1);
  CHECK_EQ((u64)result.items[0].text_edit_kind, (u64)LspCompletionTextEditKind::InsertReplace);
  CheckRange(result.items[0].insert_replace_edit.insert, 2, 1, 2, 1);
  CheckRange(result.items[0].insert_replace_edit.replace, 2, 1, 2, 4);
  CHECK_STR(result.items[0].insert_replace_edit.new_text, Str8Lit("xyz"));

  bool ok = LspDecodeCompletionResult(
      scope.arena,
      ParseJsonOrFail(scope.arena, Str8Lit("[{\"detail\":\"missing label\"}]")), &result, &error);
  CheckDecodeFailure(ok, error);
}

TEST(lsp_protocol_decodes_hover_results_and_rejects_malformed_payloads) {
  ArenaScope scope;
  String8 error = {};

  LspHover hover = {.present = true};
  CHECK(LspDecodeHover(scope.arena, ParseJsonOrFail(scope.arena, Str8Lit("null")), &hover, &error));
  CHECK(!hover.present);

  CHECK(LspDecodeHover(scope.arena,
                       ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":\"plain text\"}")),
                       &hover, &error));
  CHECK(hover.present);
  CHECK_STR(hover.text, Str8Lit("plain text"));
  CHECK(hover.kind.size == 0);
  CHECK(hover.language.size == 0);

  CHECK(LspDecodeHover(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"contents\":{\"language\":\"cpp\",\"value\":\"int x;\"}}")),
      &hover, &error));
  CHECK_STR(hover.text, Str8Lit("int x;"));
  CHECK_STR(hover.language, Str8Lit("cpp"));

  CHECK(LspDecodeHover(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"contents\":{\"kind\":\"markdown\",\"value\":\"**bold**\\nline\"},"
                              "\"range\":{\"start\":{\"line\":4,\"character\":1},"
                              "\"end\":{\"line\":4,\"character\":3}}}")),
      &hover, &error));
  CHECK_STR(hover.text, Str8Lit("**bold**\nline"));
  CHECK_STR(hover.kind, Str8Lit("markdown"));
  CHECK(hover.has_range);
  CheckRange(hover.range, 4, 1, 4, 3);

  CHECK(LspDecodeHover(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"contents\":[\"one\",{\"language\":\"cpp\",\"value\":\"two();\"}]}")),
      &hover, &error));
  CHECK_STR(hover.text, Str8Lit("one\n\ntwo();"));
  CHECK(hover.language.size == 0);

  bool ok = LspDecodeHover(scope.arena,
                           ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":{\"kind\":1}}")),
                           &hover, &error);
  CheckDecodeFailure(ok, error);
}

TEST(lsp_protocol_decodes_publish_diagnostics_and_rejects_malformed_entries) {
  ArenaScope scope;
  String8 error = {};

  JsonValue *root = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"version\":7,\"diagnostics\":["
              "{\"range\":{\"start\":{\"line\":1,\"character\":2},"
              "\"end\":{\"line\":1,\"character\":6}},\"severity\":2,\"code\":123,"
              "\"source\":\"clangd\",\"message\":\"oops\","
              "\"relatedInformation\":[{\"location\":{\"uri\":\"file:///tmp/b.cpp\","
              "\"range\":{\"start\":{\"line\":3,\"character\":4},"
              "\"end\":{\"line\":3,\"character\":9}}},\"message\":\"related\"}],"
              "\"tags\":[1,2]},"
              "{\"range\":{\"start\":{\"line\":9,\"character\":1},"
              "\"end\":{\"line\":9,\"character\":2}},\"code\":\"E0001\","
              "\"message\":\"default severity\"}]}"));
  LspPublishDiagnostics payload = {};
  CHECK(LspDecodePublishDiagnostics(scope.arena, root, &payload, &error));
  CHECK_STR(payload.uri, Str8Lit("file:///tmp/a.cpp"));
  CHECK(payload.has_version);
  CHECK_EQ(payload.version, (i64)7);
  CHECK_EQ(payload.diagnostic_count, (u64)2);
  CHECK_EQ((u64)payload.diagnostics[0].severity, (u64)LspDiagnosticSeverity::Warning);
  CHECK_STR(payload.diagnostics[0].code, Str8Lit("123"));
  CHECK_EQ(payload.diagnostics[0].related_information_count, (u64)1);
  CHECK_EQ(payload.diagnostics[0].tag_count, (u64)2);
  CHECK_EQ((u64)payload.diagnostics[1].severity, (u64)LspDiagnosticSeverity::Error);
  CHECK_STR(payload.diagnostics[1].code, Str8Lit("E0001"));

  bool ok = LspDecodePublishDiagnostics(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"diagnostics\":[{\"range\":"
                              "{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":1}},\"severity\":9,"
                              "\"message\":\"bad severity\"}]}")),
      &payload, &error);
  CheckDecodeFailure(ok, error);

  ok = LspDecodePublishDiagnostics(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"diagnostics\":[{\"message\":\"missing uri\"}]}")),
      &payload, &error);
  CheckDecodeFailure(ok, error);
}

TEST(lsp_protocol_decodes_workspace_edits_and_tracks_resource_operations) {
  ArenaScope scope;
  String8 error = {};

  JsonValue *changes = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"changes\":{\"file:///tmp/a.cpp\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":1}},\"newText\":\"A\"}],"
              "\"file:///tmp/b.cpp\":[{\"range\":{\"start\":{\"line\":1,\"character\":2},"
              "\"end\":{\"line\":1,\"character\":3}},\"newText\":\"B\"}]}}"));
  LspWorkspaceEdit edit = {};
  CHECK(LspDecodeWorkspaceEdit(scope.arena, changes, &edit, &error));
  CHECK(edit.present);
  CHECK_EQ(edit.count, (u64)2);
  CHECK_EQ((u64)edit.entries[0].kind, (u64)LspWorkspaceEditEntryKind::TextDocumentEdits);
  CHECK_STR(edit.entries[0].uri, Str8Lit("file:///tmp/a.cpp"));
  CHECK_EQ(edit.entries[0].edit_count, (u64)1);

  JsonValue *doc_changes = ParseJsonOrFail(
      scope.arena,
      Str8Lit("{\"documentChanges\":["
              "{\"textDocument\":{\"uri\":\"file:///tmp/c.cpp\",\"version\":11},"
              "\"edits\":[{\"range\":{\"start\":{\"line\":2,\"character\":1},"
              "\"end\":{\"line\":2,\"character\":2}},\"newText\":\"C\"}]},"
              "{\"kind\":\"create\",\"uri\":\"file:///tmp/new.cpp\"},"
              "{\"kind\":\"rename\",\"oldUri\":\"file:///tmp/old.cpp\",\"newUri\":\"file:///tmp/new.cpp\"},"
              "{\"kind\":\"delete\",\"uri\":\"file:///tmp/gone.cpp\"}]}"));
  edit = {};
  CHECK(LspDecodeWorkspaceEdit(scope.arena, doc_changes, &edit, &error));
  CHECK_EQ(edit.count, (u64)4);
  CHECK_EQ((u64)edit.entries[0].kind, (u64)LspWorkspaceEditEntryKind::TextDocumentEdits);
  CHECK(edit.entries[0].has_version);
  CHECK_EQ(edit.entries[0].version, (i64)11);
  CHECK_EQ((u64)edit.entries[1].kind, (u64)LspWorkspaceEditEntryKind::CreateFile);
  CHECK_EQ((u64)edit.entries[2].kind, (u64)LspWorkspaceEditEntryKind::RenameFile);
  CHECK_EQ((u64)edit.entries[3].kind, (u64)LspWorkspaceEditEntryKind::DeleteFile);

  bool ok = LspDecodeWorkspaceEdit(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"documentChanges\":[{\"textDocument\":{\"version\":1},\"edits\":[]}]}")),
      &edit, &error);
  CheckDecodeFailure(ok, error);
}

TEST(lsp_protocol_decodes_generic_text_edit_arrays_and_rejects_insert_replace_edits) {
  ArenaScope scope;
  String8 error = {};

  LspTextEdit *edits = (LspTextEdit *)1;
  u64 count = 99;
  CHECK(LspDecodeTextEdits(scope.arena, ParseJsonOrFail(scope.arena, Str8Lit("[]")), &edits, &count,
                           &error));
  CHECK(edits == nullptr);
  CHECK_EQ(count, (u64)0);

  JsonValue *root = ParseJsonOrFail(
      scope.arena,
      Str8Lit("[{\"range\":{\"start\":{\"line\":4,\"character\":5},"
              "\"end\":{\"line\":4,\"character\":6}},\"newText\":\"x\"}]"));
  CHECK(LspDecodeTextEdits(scope.arena, root, &edits, &count, &error));
  CHECK_EQ(count, (u64)1);
  CheckRange(edits[0].range, 4, 5, 4, 6);
  CHECK_STR(edits[0].new_text, Str8Lit("x"));

  bool ok = LspDecodeTextEdits(
      scope.arena,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("[{\"insert\":{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":0}},"
                              "\"replace\":{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":1}},\"newText\":\"x\"}]")),
      &edits, &count, &error);
  CheckDecodeFailure(ok, error);

  ok = LspDecodeTextEdits(scope.arena,
                          ParseJsonOrFail(scope.arena, Str8Lit("[{\"range\":{},\"newText\":\"x\"}]")),
                          &edits, &count, &error);
  CheckDecodeFailure(ok, error);
}
