#include "editor/lsp_actions.h"

#include "editor/editor.h"
#include "editor/lsp.h"
#include "editor/view.h"
#include "lsp/json.h"
#include "os/os_file.h"
#include "test.h"

#include <chrono>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct ArenaScope {
  Arena *arena;

  explicit ArenaScope(u64 reserve_size = MB(32)) : arena(ArenaAlloc(reserve_size)) {}
  ~ArenaScope() { ArenaRelease(arena); }
};

struct ScopedFixtureDir {
  Arena *arena;
  String8 path;

  ScopedFixtureDir(Arena *arena_, const char *tag) : arena(arena_) {
    static i32 serial = 0;
    serial += 1;
    String8 cwd = OsGetCwd(arena);
    path = PushStr8F(arena, "%.*s/build/test_lsp_actions_%s_%d", (int)cwd.size, (char *)cwd.str,
                     tag, (int)serial);
    (void)OsDirDeleteRecursive(path);
    CHECK(OsMakeDirs(path));
  }

  ~ScopedFixtureDir() { (void)OsDirDeleteRecursive(path); }
};

struct EditorScope {
  Editor ed;

  explicit EditorScope(Arena *arena) { EditorInit(&ed, arena, RectS32{0, 0, 80, 25}); }
  ~EditorScope() { EditorDestroy(&ed); }
};

struct ResolveEntry {
  std::string root;
  std::string script_path;
};

struct ResolveState {
  std::vector<ResolveEntry> entries;
};

struct RenameCapture {
  bool called = false;
  EditorLspRenamePrepareResult result = {};
  std::string placeholder;
  std::string prompt;
};

String8 WriteTextFile(Arena *arena, String8 dir, String8 name, String8 text) {
  String8 path = OsPathJoin(arena, dir, name);
  CHECK(OsFileWrite(path, text));
  return path;
}

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

template <typename Fn>
bool WaitUntil(Fn predicate, i32 timeout_ms = 1500) {
  Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

bool StatusContains(Editor *ed, const char *needle) {
  if (ed == nullptr || ed->status_message.str == nullptr) return false;
  String8 status = ed->status_message;
  String8 text = Str8C(needle);
  if (text.size == 0 || status.size < text.size) return false;
  for (u64 i = 0; i + text.size <= status.size; i += 1) {
    if (memcmp(status.str + i, text.str, (size_t)text.size) == 0) return true;
  }
  return false;
}

String8 BufferTextAllOrFail(Arena *arena, Buffer *buffer) {
  CHECK(buffer != nullptr);
  return BufferTextAll(arena, buffer);
}

Buffer *OpenFileBuffer(Editor *ed, String8 path) {
  BufferHandle handle = EditorOpenFile(ed, path);
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  return buffer;
}

void FocusBufferAt(Editor *ed, Buffer *buffer, u64 cursor) {
  View *view = EditorFocusedView(ed);
  CHECK(view != nullptr);
  view->buffer = buffer->handle;
  ViewSetCursor(view, buffer, cursor);
}

LspClientResponse OkResponse(i64 version, const JsonValue *result) {
  LspClientResponse response = {};
  response.context.document_version = version;
  response.has_result = true;
  response.result = result;
  return response;
}

void CaptureRenamePrepare(void *user_data, const EditorLspRenamePrepareResult *result) {
  RenameCapture *capture = (RenameCapture *)user_data;
  CHECK(capture != nullptr);
  CHECK(result != nullptr);
  capture->called = true;
  capture->result = *result;
  capture->placeholder.assign((const char *)result->placeholder.str, (size_t)result->placeholder.size);
  capture->prompt.assign((const char *)result->prompt.str, (size_t)result->prompt.size);
}

bool ResolveFakeServer(Arena *arena, LspLanguage language, String8 language_id, String8 root,
                       LspServerCommand *command, void *user_data) {
  (void)language;
  ResolveState *state = (ResolveState *)user_data;
  if (state == nullptr || command == nullptr) return false;

  std::string root_key((const char *)root.str, (size_t)root.size);
  for (const ResolveEntry &entry : state->entries) {
    if (entry.root != root_key) continue;
    *command = {};
    command->language = language;
    command->language_id = PushStr8Copy(arena, language_id);
    command->executable = PushStr8Copy(arena, Str8C(EDITOR_FAKE_LSP_PATH));
    command->arguments = PushArray(arena, String8, 2);
    command->arguments[0] = Str8Lit("--script");
    command->arguments[1] = PushStr8Copy(arena, Str8C(entry.script_path.c_str()));
    command->argument_count = 2;
    command->root = PushStr8Copy(arena, root);
    return true;
  }

  return false;
}

EditorLspConfig MakeLspConfig(ResolveState *state) {
  EditorLspConfig config = {};
  config.resolve_command = ResolveFakeServer;
  config.resolve_command_user_data = state;
  config.shutdown_timeout_ms = 60;
  return config;
}

String8 MakeInitializeResult(Arena *arena, String8 capabilities_json) {
  return PushStr8F(arena, "{\"capabilities\":%.*s}", (int)capabilities_json.size,
                   (char *)capabilities_json.str);
}

String8 WriteSessionScript(Arena *arena, String8 dir, String8 name, String8 record_path,
                           String8 initialize_result,
                           std::initializer_list<String8> methods_after_initialized) {
  JsonWriter writer = {};
  JsonWriterInit(&writer, arena);
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("recordPath")));
  CHECK(JsonWriteString(&writer, record_path));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("onMessage")));
  CHECK(JsonWriteArrayBegin(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("initialize")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&writer,
                        PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":%.*s}",
                                  (int)initialize_result.size, (char *)initialize_result.str)));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("initialized")));
  CHECK(JsonWriteObjectEnd(&writer));

  for (String8 method : methods_after_initialized) {
    CHECK(JsonWriteObjectBegin(&writer));
    CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
    CHECK(JsonWriteString(&writer, method));
    if (Str8Match(method, Str8Lit("shutdown"))) {
      CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
      CHECK(JsonWriteArrayBegin(&writer));
      CHECK(JsonWriteObjectBegin(&writer));
      CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
      CHECK(JsonWriteString(&writer, Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}")));
      CHECK(JsonWriteObjectEnd(&writer));
      CHECK(JsonWriteArrayEnd(&writer));
    }
    CHECK(JsonWriteObjectEnd(&writer));
  }

  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));
  String8 script = FinishJsonWriterOrFail(&writer);
  JsonWriterDestroy(&writer);
  return WriteTextFile(arena, dir, name, script);
}

u64 RecordedMessageCount(String8 path) {
  TempArena scratch = ScratchBegin();
  FileContents contents = OsFileRead(scratch.arena, path);
  if (!contents.ok || contents.data.size == 0) {
    ScratchEnd(scratch);
    return 0;
  }

  String8List lines = Str8SplitChar(scratch.arena, contents.data, '\n');
  u64 count = 0;
  for (String8Node *node = lines.first; node != nullptr; node = node->next) {
    if (node->string.size > 0) count += 1;
  }
  ScratchEnd(scratch);
  return count;
}

JsonValue *RecordedMessageAt(Arena *arena, String8 path, u64 index) {
  FileContents contents = OsFileRead(arena, path);
  CHECK(contents.ok);

  String8List lines = Str8SplitChar(arena, contents.data, '\n');
  u64 current = 0;
  for (String8Node *node = lines.first; node != nullptr; node = node->next) {
    if (node->string.size == 0) continue;
    if (current == index) return ParseJsonOrFail(arena, node->string);
    current += 1;
  }

  TestFail(__FILE__, __LINE__, "recorded message %llu missing", (unsigned long long)index);
  return nullptr;
}

void CheckRecordedMethod(JsonValue *root, String8 want_method) {
  String8 got_method = {};
  CHECK(JsonGetString(JsonObjectGet(root, Str8Lit("method")), &got_method));
  CHECK_STR(got_method, want_method);
}

TEST(lsp_actions_formatting_response_applies_edits_in_reverse_order) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "format_reverse");
  EditorScope editor(scope.arena);

  String8 path = WriteTextFile(scope.arena, fixture.path, Str8Lit("main.cpp"), Str8Lit("hello world\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(
      scratch.arena,
      Str8Lit("[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":5}},"
              "\"newText\":\"hi\"},"
              "{\"range\":{\"start\":{\"line\":0,\"character\":6},\"end\":{\"line\":0,\"character\":11}},"
              "\"newText\":\"earth\"}]"));
  String8 uri = LspPathToUri(scope.arena, path);
  EditorLspFormatContext context = {};
  context.ed = &editor.ed;
  context.buffer = buffer->handle;
  context.uri = uri;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)buffer->edit_serial, result);
  EditorLspOnFormattingResponse(&context, &response);
  ScratchEnd(scratch);

  CHECK_STR(BufferTextAllOrFail(scope.arena, buffer), Str8Lit("hi earth\n"));
  CHECK(BufferIsDirty(buffer));
}

TEST(lsp_actions_formatting_response_rejects_overlapping_edits) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "format_overlap");
  EditorScope editor(scope.arena);

  String8 path = WriteTextFile(scope.arena, fixture.path, Str8Lit("main.cpp"), Str8Lit("abcdef\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(
      scratch.arena,
      Str8Lit("[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":3}},"
              "\"newText\":\"x\"},"
              "{\"range\":{\"start\":{\"line\":0,\"character\":2},\"end\":{\"line\":0,\"character\":4}},"
              "\"newText\":\"y\"}]"));
  String8 uri = LspPathToUri(scope.arena, path);
  EditorLspFormatContext context = {};
  context.ed = &editor.ed;
  context.buffer = buffer->handle;
  context.uri = uri;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)buffer->edit_serial, result);
  EditorLspOnFormattingResponse(&context, &response);
  ScratchEnd(scratch);

  CHECK_STR(BufferTextAllOrFail(scope.arena, buffer), Str8Lit("abcdef\n"));
  CHECK(StatusContains(&editor.ed, "overlap"));
}

TEST(lsp_actions_formatting_response_ignores_stale_results) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "format_stale");
  EditorScope editor(scope.arena);

  String8 path = WriteTextFile(scope.arena, fixture.path, Str8Lit("main.cpp"), Str8Lit("int x = 1;\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);
  BufferInsert(&editor.ed, buffer, BufferSize(buffer), Str8Lit("// local\n"), BufferSize(buffer),
               BufferSize(buffer));

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(
      scratch.arena,
      Str8Lit("[{\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,\"character\":5}},"
              "\"newText\":\"y\"}]"));
  String8 uri = LspPathToUri(scope.arena, path);
  EditorLspFormatContext context = {};
  context.ed = &editor.ed;
  context.buffer = buffer->handle;
  context.uri = uri;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)buffer->edit_serial - 1, result);
  EditorLspOnFormattingResponse(&context, &response);
  ScratchEnd(scratch);

  CHECK(StatusContains(&editor.ed, "stale"));
  CHECK_STR(BufferTextAllOrFail(scope.arena, buffer), Str8Lit("int x = 1;\n// local\n"));
}

TEST(lsp_actions_navigation_requests_map_each_kind_to_its_method) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "nav_methods");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));
  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("record.jsonl"));
  String8 script_path = WriteSessionScript(
      scope.arena, fixture.path, Str8Lit("session.json"), record_path,
      MakeInitializeResult(
          scope.arena,
          Str8Lit("{\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
                  "\"declarationProvider\":true,\"definitionProvider\":true,"
                  "\"implementationProvider\":true,\"typeDefinitionProvider\":true}")),
      {Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/declaration"),
       Str8Lit("textDocument/definition"), Str8Lit("textDocument/implementation"),
       Str8Lit("textDocument/typeDefinition"), Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script_path.str, (size_t)script_path.size)});

  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  String8 path = WriteTextFile(scope.arena, root, Str8Lit("main.cpp"), Str8Lit("call();\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);
  FocusBufferAt(&editor.ed, buffer, 1);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return RecordedMessageCount(record_path) >= 3;
  }));

  CHECK(EditorLspRequestNavigation(&editor.ed, buffer, EditorLspNavigationKind::Declaration) != 0);
  CHECK(EditorLspRequestNavigation(&editor.ed, buffer, EditorLspNavigationKind::Definition) != 0);
  CHECK(EditorLspRequestNavigation(&editor.ed, buffer, EditorLspNavigationKind::Implementation) != 0);
  CHECK(EditorLspRequestNavigation(&editor.ed, buffer, EditorLspNavigationKind::TypeDefinition) != 0);

  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return RecordedMessageCount(record_path) >= 7;
  }));

  JsonValue *decl = RecordedMessageAt(scope.arena, record_path, 3);
  JsonValue *defn = RecordedMessageAt(scope.arena, record_path, 4);
  JsonValue *impl = RecordedMessageAt(scope.arena, record_path, 5);
  JsonValue *type = RecordedMessageAt(scope.arena, record_path, 6);
  CheckRecordedMethod(decl, Str8Lit("textDocument/declaration"));
  CheckRecordedMethod(defn, Str8Lit("textDocument/definition"));
  CheckRecordedMethod(impl, Str8Lit("textDocument/implementation"));
  CheckRecordedMethod(type, Str8Lit("textDocument/typeDefinition"));
}

TEST(lsp_actions_navigation_response_pushes_jump_opens_target_and_reports_multi_count) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "nav_response");
  EditorScope editor(scope.arena);

  String8 source_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("source.cpp"),
                                      Str8Lit("alpha();\n"));
  String8 target_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("target.cpp"),
                                      Str8Lit("one\ncall_here();\nthree\n"));
  Buffer *source = OpenFileBuffer(&editor.ed, source_path);
  FocusBufferAt(&editor.ed, source, 2);

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(
      scratch.arena,
      PushStr8F(scratch.arena,
                "[{\"uri\":\"%.*s\",\"range\":{\"start\":{\"line\":1,\"character\":0},"
                "\"end\":{\"line\":1,\"character\":4}}},"
                "{\"targetUri\":\"%.*s\",\"targetRange\":{\"start\":{\"line\":2,\"character\":0},"
                "\"end\":{\"line\":2,\"character\":1}}}]",
                (int)LspPathToUri(scratch.arena, target_path).size,
                (char *)LspPathToUri(scratch.arena, target_path).str,
                (int)LspPathToUri(scratch.arena, target_path).size,
                (char *)LspPathToUri(scratch.arena, target_path).str));
  EditorLspNavigationContext context = {};
  context.ed = &editor.ed;
  context.buffer = source->handle;
  context.uri = LspPathToUri(scope.arena, source_path);
  context.kind = EditorLspNavigationKind::Definition;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)source->edit_serial, result);
  EditorLspOnNavigationResponse(&context, &response);
  ScratchEnd(scratch);

  View *view = EditorFocusedView(&editor.ed);
  Buffer *shown = EditorFocusedBuffer(&editor.ed);
  CHECK(view != nullptr);
  CHECK(shown != nullptr);
  CHECK_STR(shown->path, target_path);
  CHECK_EQ(ViewCursorLine(view, shown), (u64)1);
  CHECK_EQ(ViewCursorColumn(view, shown), (u64)0);
  CHECK(StatusContains(&editor.ed, "2 result"));
  CHECK(EditorJumpOlder(&editor.ed, view, 1));
  CHECK(BufferHandleEqual(view->buffer, source->handle));
  CHECK_EQ(view->cursor, (u64)2);
}

TEST(lsp_actions_prepare_rename_response_uses_placeholder_and_range) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_prepare");
  EditorScope editor(scope.arena);

  String8 path = WriteTextFile(scope.arena, fixture.path, Str8Lit("main.cpp"), Str8Lit("value = 1;\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);
  RenameCapture capture = {};

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(
      scratch.arena,
      Str8Lit("{\"range\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":5}},\"placeholder\":\"value\"}"));
  EditorLspRenamePrepareContext context = {};
  context.ed = &editor.ed;
  context.buffer = buffer->handle;
  context.uri = LspPathToUri(scope.arena, path);
  context.callback = CaptureRenamePrepare;
  context.callback_user_data = &capture;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)buffer->edit_serial, result);
  EditorLspOnPrepareRenameResponse(&context, &response);
  ScratchEnd(scratch);

  CHECK(capture.called);
  CHECK_EQ((u64)capture.result.status, (u64)EditorLspRenamePrepareStatus::Ready);
  CHECK_EQ(capture.result.range.min, (u64)0);
  CHECK_EQ(capture.result.range.max, (u64)5);
  CHECK_STR(Str8C(capture.placeholder.c_str()), Str8Lit("value"));
  CHECK_STR(Str8C(capture.prompt.c_str()), Str8Lit("value"));
}

TEST(lsp_actions_prepare_rename_falls_back_to_identifier_without_prepare_support) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_fallback");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));
  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("record.jsonl"));
  String8 script_path = WriteSessionScript(
      scope.arena, fixture.path, Str8Lit("session.json"), record_path,
      MakeInitializeResult(scope.arena,
                           Str8Lit("{\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
                                   "\"renameProvider\":true}")),
      {Str8Lit("textDocument/didOpen"), Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script_path.str, (size_t)script_path.size)});

  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  String8 path = WriteTextFile(scope.arena, root, Str8Lit("main.cpp"), Str8Lit("  symbol_name();\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);
  FocusBufferAt(&editor.ed, buffer, 0);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return RecordedMessageCount(record_path) >= 3;
  }));

  RenameCapture capture = {};
  u64 before = RecordedMessageCount(record_path);
  CHECK(EditorLspPrepareRename(&editor.ed, buffer, CaptureRenamePrepare, &capture) == 0);
  CHECK(capture.called);
  CHECK_EQ((u64)capture.result.status, (u64)EditorLspRenamePrepareStatus::Ready);
  CHECK_STR(Str8C(capture.prompt.c_str()), Str8Lit("symbol_name"));
  CHECK_EQ(capture.result.range.min, (u64)2);
  CHECK_EQ(capture.result.range.max, (u64)13);
  CHECK_EQ(RecordedMessageCount(record_path), before);
}

TEST(lsp_actions_prepare_rename_response_reports_rejection) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_reject");
  EditorScope editor(scope.arena);

  String8 path = WriteTextFile(scope.arena, fixture.path, Str8Lit("main.cpp"), Str8Lit("value = 1;\n"));
  Buffer *buffer = OpenFileBuffer(&editor.ed, path);
  RenameCapture capture = {};

  TempArena scratch = ScratchBegin();
  JsonValue *result = ParseJsonOrFail(scratch.arena, Str8Lit("null"));
  EditorLspRenamePrepareContext context = {};
  context.ed = &editor.ed;
  context.buffer = buffer->handle;
  context.uri = LspPathToUri(scope.arena, path);
  context.callback = CaptureRenamePrepare;
  context.callback_user_data = &capture;
  context.position_encoding = LspPositionEncoding::Utf8;
  LspClientResponse response = OkResponse((i64)buffer->edit_serial, result);
  EditorLspOnPrepareRenameResponse(&context, &response);
  ScratchEnd(scratch);

  CHECK(capture.called);
  CHECK_EQ((u64)capture.result.status, (u64)EditorLspRenamePrepareStatus::Rejected);
  CHECK(StatusContains(&editor.ed, "rename"));
}

TEST(lsp_actions_workspace_edit_applies_changes_to_open_and_new_buffers) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "workspace_changes");
  EditorScope editor(scope.arena);

  String8 open_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("open.cpp"), Str8Lit("alpha\n"));
  String8 closed_path =
      WriteTextFile(scope.arena, fixture.path, Str8Lit("closed.cpp"), Str8Lit("beta\ngamma\n"));
  Buffer *open_buffer = OpenFileBuffer(&editor.ed, open_path);

  TempArena scratch = ScratchBegin();
  JsonValue *root = ParseJsonOrFail(
      scratch.arena,
      PushStr8F(
          scratch.arena,
          "{\"changes\":{\"%.*s\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
          "\"end\":{\"line\":0,\"character\":5}},\"newText\":\"ALPHA\"}],"
          "\"%.*s\":[{\"range\":{\"start\":{\"line\":1,\"character\":0},"
          "\"end\":{\"line\":1,\"character\":5}},\"newText\":\"GAMMA\"}]}}",
          (int)LspPathToUri(scratch.arena, open_path).size,
          (char *)LspPathToUri(scratch.arena, open_path).str,
          (int)LspPathToUri(scratch.arena, closed_path).size,
          (char *)LspPathToUri(scratch.arena, closed_path).str));
  String8 error = {};
  LspWorkspaceEdit edit = {};
  CHECK(LspDecodeWorkspaceEdit(scratch.arena, root, &edit, &error));
  CHECK(EditorLspApplyWorkspaceEdit(&editor.ed, &edit, LspPositionEncoding::Utf8));
  ScratchEnd(scratch);

  Buffer *closed_buffer = BufferFromHandle(&editor.ed.buffers, BufferFromPath(&editor.ed.buffers, closed_path));
  CHECK(closed_buffer != nullptr);
  CHECK_STR(BufferTextAllOrFail(scope.arena, open_buffer), Str8Lit("ALPHA\n"));
  CHECK_STR(BufferTextAllOrFail(scope.arena, closed_buffer), Str8Lit("beta\nGAMMA\n"));
  CHECK(BufferIsDirty(open_buffer));
  CHECK(BufferIsDirty(closed_buffer));
}

TEST(lsp_actions_workspace_edit_enforces_versions_atomically) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "workspace_versions");
  EditorScope editor(scope.arena);

  String8 a_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("a.cpp"), Str8Lit("one\n"));
  String8 b_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("b.cpp"), Str8Lit("two\n"));
  Buffer *a_buffer = OpenFileBuffer(&editor.ed, a_path);
  Buffer *b_buffer = OpenFileBuffer(&editor.ed, b_path);

  TempArena scratch = ScratchBegin();
  JsonValue *root = ParseJsonOrFail(
      scratch.arena,
      PushStr8F(
          scratch.arena,
          "{\"documentChanges\":["
          "{\"textDocument\":{\"uri\":\"%.*s\",\"version\":%lld},"
          "\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
          "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"ONE\"}]},"
          "{\"textDocument\":{\"uri\":\"%.*s\",\"version\":%lld},"
          "\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
          "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"TWO\"}]}]}",
          (int)LspPathToUri(scratch.arena, a_path).size,
          (char *)LspPathToUri(scratch.arena, a_path).str, (long long)a_buffer->edit_serial,
          (int)LspPathToUri(scratch.arena, b_path).size,
          (char *)LspPathToUri(scratch.arena, b_path).str,
          (long long)b_buffer->edit_serial + 99));
  String8 error = {};
  LspWorkspaceEdit edit = {};
  CHECK(LspDecodeWorkspaceEdit(scratch.arena, root, &edit, &error));
  CHECK(!EditorLspApplyWorkspaceEdit(&editor.ed, &edit, LspPositionEncoding::Utf8));
  ScratchEnd(scratch);

  CHECK(StatusContains(&editor.ed, "version"));
  CHECK_STR(BufferTextAllOrFail(scope.arena, a_buffer), Str8Lit("one\n"));
  CHECK_STR(BufferTextAllOrFail(scope.arena, b_buffer), Str8Lit("two\n"));
}

TEST(lsp_actions_workspace_edit_rejects_resource_operations) {
  ArenaScope scope;
  EditorScope editor(scope.arena);

  TempArena scratch = ScratchBegin();
  JsonValue *root = ParseJsonOrFail(
      scratch.arena,
      Str8Lit("{\"documentChanges\":[{\"kind\":\"create\",\"uri\":\"file:///tmp/new.cpp\"}]}"));
  String8 error = {};
  LspWorkspaceEdit edit = {};
  CHECK(LspDecodeWorkspaceEdit(scratch.arena, root, &edit, &error));
  CHECK(!EditorLspApplyWorkspaceEdit(&editor.ed, &edit, LspPositionEncoding::Utf8));
  ScratchEnd(scratch);

  CHECK(StatusContains(&editor.ed, "resource"));
}

}  // namespace
