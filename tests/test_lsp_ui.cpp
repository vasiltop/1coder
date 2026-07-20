#include "editor/editor.h"
#include "editor/lsp_ui.h"
#include "lsp/json.h"
#include "lsp/protocol.h"
#include "test.h"

namespace {

struct ArenaScope {
  Arena *arena;

  explicit ArenaScope(u64 reserve_size = MB(16)) : arena(ArenaAlloc(reserve_size)) {}
  ~ArenaScope() { ArenaRelease(arena); }
};

struct EditorScope {
  Editor ed;

  explicit EditorScope(Arena *arena) { EditorInit(&ed, arena, RectS32{0, 0, 80, 25}); }
  ~EditorScope() { EditorDestroy(&ed); }
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

String8 FinishJsonWriterOrFail(JsonWriter *writer) {
  String8 json = JsonWriterFinish(writer);
  if (json.size == 0) {
    String8 error = JsonWriterError(writer);
    TestFail(__FILE__, __LINE__, "json writer failed: %.*s", (int)error.size, (char *)error.str);
  }
  return json;
}

Buffer *OpenFileBuffer(Editor *ed, const char *name, const char *path, const char *text) {
  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::File, Str8C(name));
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  buffer->path = PushStr8Copy(buffer->arena, Str8C(path));
  BufferSetText(ed, buffer, Str8C(text));
  return buffer;
}

View *FocusBuffer(Editor *ed, Buffer *buffer, u64 cursor) {
  View *view = EditorFocusedView(ed);
  CHECK(view != nullptr);
  view->buffer = buffer->handle;
  ViewSetCursor(view, buffer, cursor);
  return view;
}

String8 UriFor(Arena *arena, Buffer *buffer) { return LspPathToUri(arena, buffer->path); }

const EditorLspUiPopupView *Popup(EditorLspUi *ui) { return EditorLspUiPopup(ui); }

TEST(lsp_ui_diagnostics_replace_clear_lookup_and_close) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "a.cpp", "/tmp/a.cpp", "abcd\nsecond\n");

  String8 error = {};
  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"version\":1,\"diagnostics\":["
                              "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":4}},\"severity\":2,"
                              "\"source\":\"clangd\",\"code\":\"W1\",\"message\":\"warn\"},"
                              "{\"range\":{\"start\":{\"line\":0,\"character\":1},"
                              "\"end\":{\"line\":0,\"character\":2}},\"severity\":1,"
                              "\"source\":\"clangd\",\"code\":\"E1\",\"message\":\"error\"}]}")),
      &error));

  u64 count = 0;
  const LspDiagnostic *diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  CHECK_EQ(count, (u64)2);
  CHECK(diagnostics != nullptr);
  CHECK_STR(diagnostics[0].message, Str8Lit("warn"));
  CHECK_STR(diagnostics[1].message, Str8Lit("error"));

  diagnostics = EditorLspUiDiagnosticsForUri(ui, UriFor(scope.arena, buffer), &count);
  CHECK_EQ(count, (u64)2);
  CHECK_EQ((u64)EditorLspUiHighestSeverityForLine(ui, buffer, 0),
           (u64)LspDiagnosticSeverity::Error);
  CHECK_EQ((u64)EditorLspUiHighestSeverityForLine(ui, buffer, 1), (u64)0);

  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"version\":0,\"diagnostics\":[]}")),
      &error));
  diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  CHECK_EQ(count, (u64)2);

  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"version\":2,\"diagnostics\":[]}")),
      &error));
  diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  CHECK_EQ(count, (u64)0);

  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/a.cpp\",\"diagnostics\":["
                              "{\"range\":{\"start\":{\"line\":1,\"character\":0},"
                              "\"end\":{\"line\":1,\"character\":6}},\"severity\":3,"
                              "\"message\":\"note\"}]}")),
      &error));
  diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  CHECK_EQ(count, (u64)1);

  EditorLspUiOnBufferClosed(ui, buffer);
  diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &count);
  CHECK_EQ(count, (u64)0);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_completion_handles_null_stale_and_stable_sort) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "b.cpp", "/tmp/b.cpp", "abc");

  String8 error = {};
  i64 version = (i64)buffer->edit_serial;
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, BufferSize(buffer), version,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"items\":["
                              "{\"label\":\"plain\"},"
                              "{\"label\":\"z\",\"sortText\":\"002\"},"
                              "{\"label\":\"a\",\"sortText\":\"001\"},"
                              "{\"label\":\"b\",\"sortText\":\"001\"}]}")),
      &error));

  const EditorLspUiPopupView *popup = Popup(ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Completion);
  CHECK_EQ(popup->completion.count, (u64)4);
  CHECK_STR(popup->completion.items[0].label, Str8Lit("a"));
  CHECK_STR(popup->completion.items[1].label, Str8Lit("b"));
  CHECK_STR(popup->completion.items[2].label, Str8Lit("z"));
  CHECK_STR(popup->completion.items[3].label, Str8Lit("plain"));

  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, BufferSize(buffer), version,
      ParseJsonOrFail(scope.arena, Str8Lit("null")), &error));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  BufferInsert(&editor.ed, buffer, BufferSize(buffer), Str8Lit("!"), BufferSize(buffer),
               BufferSize(buffer) + 1);
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, BufferSize(buffer), version,
      ParseJsonOrFail(scope.arena, Str8Lit("[{\"label\":\"stale\"}]")), &error));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_completion_popup_input_wraps_and_dismisses) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "c.cpp", "/tmp/c.cpp", "abc");
  View *view = FocusBuffer(&editor.ed, buffer, BufferSize(buffer));

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, view->cursor, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("[{\"label\":\"one\"},{\"label\":\"two\"},{\"label\":\"three\"}]")),
      &error));

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::Up)));
  CHECK_EQ(Popup(ui)->completion.selected, (u64)2);

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::Down)));
  CHECK_EQ(Popup(ui)->completion.selected, (u64)0);

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::N, KeyMod::Ctrl)));
  CHECK_EQ(Popup(ui)->completion.selected, (u64)1);

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::P, KeyMod::Ctrl)));
  CHECK_EQ(Popup(ui)->completion.selected, (u64)0);

  CHECK(!EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordChar('x')));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_completion_accepts_text_edits_atomically) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "d.cpp", "/tmp/d.cpp", "name\nbody\n");
  View *view = FocusBuffer(&editor.ed, buffer, BufferOffsetFromLine(buffer, 1));

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, view->cursor, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("[{\"label\":\"printf\",\"textEdit\":{\"range\":{\"start\":"
                              "{\"line\":1,\"character\":0},\"end\":{\"line\":1,"
                              "\"character\":4}},\"newText\":\"printf()\"},"
                              "\"additionalTextEdits\":[{\"range\":{\"start\":{\"line\":0,"
                              "\"character\":0},\"end\":{\"line\":0,\"character\":0}},"
                              "\"newText\":\"//\\n\"}]}]")),
      &error));

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::Return)));
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("//\nname\nprintf()\n"));
  CHECK_EQ(ViewCursorLine(view, buffer), (u64)2);
  CHECK_EQ(ViewCursorColumn(view, buffer), (u64)8);
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_completion_accepts_insert_replace_edits) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "e.cpp", "/tmp/e.cpp", "abzz\n");
  View *view = FocusBuffer(&editor.ed, buffer, 2);

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, view->cursor, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("[{\"label\":\"cd\",\"textEdit\":{\"insert\":{\"start\":"
                              "{\"line\":0,\"character\":2},\"end\":{\"line\":0,"
                              "\"character\":2}},\"replace\":{\"start\":{\"line\":0,"
                              "\"character\":2},\"end\":{\"line\":0,\"character\":4}},"
                              "\"newText\":\"cd\"}}]")),
      &error));

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::Return)));
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("abcd\n"));
  CHECK_EQ(ViewCursorColumn(view, buffer), (u64)4);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_completion_rejects_overlapping_additional_edits) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "f.cpp", "/tmp/f.cpp", "abc\n");
  View *view = FocusBuffer(&editor.ed, buffer, 0);

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      ui, buffer, LspPositionEncoding::Utf8, view->cursor, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("[{\"label\":\"x\",\"textEdit\":{\"range\":{\"start\":{\"line\":0,"
                              "\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
                              "\"newText\":\"x\"},\"additionalTextEdits\":[{\"range\":"
                              "{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,"
                              "\"character\":1}},\"newText\":\"y\"}]}]")),
      &error));

  CHECK(EditorLspUiHandlePopupInput(ui, &editor.ed, view, KeyChordKey(Key::Return)));
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("abc\n"));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_hover_normalizes_content_forms) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "g.cpp", "/tmp/g.cpp", "value\n");

  String8 error = {};
  CHECK(EditorLspUiShowHoverResult(
      ui, buffer, LspPositionEncoding::Utf8, 0, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"contents\":[\"one\",{\"language\":\"cpp\","
                              "\"value\":\"two();\"}]}")),
      &error));
  const EditorLspUiPopupView *popup = Popup(ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Hover);
  CHECK_EQ(popup->text.line_count, (u64)3);
  CHECK_STR(popup->text.lines[0], Str8Lit("one"));
  CHECK_STR(popup->text.lines[1], Str8Lit(""));
  CHECK_STR(popup->text.lines[2], Str8Lit("two();"));

  CHECK(EditorLspUiShowHoverResult(
      ui, buffer, LspPositionEncoding::Utf8, 0, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"contents\":{\"kind\":\"markdown\",\"value\":\"**bold**\\nline\"},"
                              "\"range\":{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":5}}}")),
      &error));
  popup = Popup(ui);
  CHECK(popup->text.has_range);
  CHECK_STR(popup->text.lines[0], Str8Lit("**bold**"));
  CHECK_STR(popup->text.lines[1], Str8Lit("line"));

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_hover_bounds_and_stale_responses) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "h.cpp", "/tmp/h.cpp", "value\n");

  String8List lines = {};
  for (u64 i = 0; i < 205; i += 1) {
    Str8ListPush(scope.arena, &lines,
                 PushStr8F(scope.arena,
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx%llu",
                           (unsigned long long)i));
  }
  String8 hover_text = Str8ListJoin(scope.arena, &lines, Str8Lit("\n"));
  JsonWriter writer = {};
  JsonWriterInit(&writer, scope.arena);
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("contents")));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("kind")));
  CHECK(JsonWriteString(&writer, Str8Lit("markdown")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("value")));
  CHECK(JsonWriteString(&writer, hover_text));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));
  String8 hover_json = FinishJsonWriterOrFail(&writer);
  JsonWriterDestroy(&writer);

  String8 error = {};
  i64 version = (i64)buffer->edit_serial;
  CHECK(EditorLspUiShowHoverResult(ui, buffer, LspPositionEncoding::Utf8, 0, version,
                                   ParseJsonOrFail(scope.arena, hover_json), &error));
  const EditorLspUiPopupView *popup = Popup(ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Hover);
  CHECK_EQ(popup->text.line_count, (u64)200);
  CHECK_EQ(popup->text.lines[0].size, (u64)512);
  CHECK_STR(popup->text.lines[199], Str8Lit("[truncated]"));

  BufferInsert(&editor.ed, buffer, 0, Str8Lit("!"), 0, 1);
  CHECK(EditorLspUiShowHoverResult(
      ui, buffer, LspPositionEncoding::Utf8, 0, version,
      ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":\"stale\"}")), &error));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_diagnostic_popup_collects_multiple_entries_and_reports_empty) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *buffer = OpenFileBuffer(&editor.ed, "i.cpp", "/tmp/i.cpp", "abcd\n");
  View *view = FocusBuffer(&editor.ed, buffer, 1);

  String8 error = {};
  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena,
                      Str8Lit("{\"uri\":\"file:///tmp/i.cpp\",\"diagnostics\":["
                              "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                              "\"end\":{\"line\":0,\"character\":4}},\"severity\":1,"
                              "\"source\":\"clangd\",\"code\":\"E1\",\"message\":\"high\"},"
                              "{\"range\":{\"start\":{\"line\":0,\"character\":1},"
                              "\"end\":{\"line\":0,\"character\":2}},\"severity\":2,"
                              "\"message\":\"low\"}]}")),
      &error));

  String8 status = {};
  CHECK(EditorLspUiBuildDiagnosticPopup(ui, buffer, view->cursor, &status));
  const EditorLspUiPopupView *popup = Popup(ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Diagnostic);
  CHECK_EQ(popup->text.line_count, (u64)2);
  CHECK_STR(popup->text.lines[0], Str8Lit("error: high (clangd E1)"));
  CHECK_STR(popup->text.lines[1], Str8Lit("warning: low"));

  CHECK(EditorLspUiApplyPublishDiagnostics(
      ui, &editor.ed,
      ParseJsonOrFail(scope.arena, Str8Lit("{\"uri\":\"file:///tmp/i.cpp\",\"diagnostics\":[]}")),
      &error));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  CHECK(!EditorLspUiBuildDiagnosticPopup(ui, buffer, BufferSize(buffer), &status));
  CHECK_STR(status, Str8Lit("No diagnostics at cursor"));
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

TEST(lsp_ui_popup_invalidates_on_buffer_or_version_change) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  EditorLspUi *ui = EditorLspUiCreate();
  Buffer *first = OpenFileBuffer(&editor.ed, "j.cpp", "/tmp/j.cpp", "abc");
  Buffer *second = OpenFileBuffer(&editor.ed, "k.cpp", "/tmp/k.cpp", "def");

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      ui, first, LspPositionEncoding::Utf8, BufferSize(first), (i64)first->edit_serial,
      ParseJsonOrFail(scope.arena, Str8Lit("[{\"label\":\"one\"}]")), &error));
  EditorLspUiInvalidatePopupIfStale(ui, first);
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::Completion);

  BufferInsert(&editor.ed, first, BufferSize(first), Str8Lit("!"), BufferSize(first),
               BufferSize(first) + 1);
  EditorLspUiInvalidatePopupIfStale(ui, first);
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  CHECK(EditorLspUiShowHoverResult(
      ui, first, LspPositionEncoding::Utf8, 0, (i64)first->edit_serial,
      ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":\"hover\"}")), &error));
  EditorLspUiInvalidatePopupIfStale(ui, second);
  CHECK_EQ((u64)Popup(ui)->kind, (u64)EditorLspUiPopupKind::None);

  EditorLspUiDestroy(ui);
}

}  // namespace
