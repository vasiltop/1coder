#include "editor/command.h"
#include "editor/editor.h"
#include "editor/lsp.h"
#include "editor/lsp_ui.h"
#include "input/keymap.h"
#include "lsp/json.h"
#include "os/os_file.h"
#include "test.h"

#include <chrono>
#include <cstring>
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
    path = PushStr8F(arena, "%.*s/build/test_lsp_wiring_%s_%d", (int)cwd.size, (char *)cwd.str,
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

u64 FindRecordedMethod(Arena *arena, String8 path, String8 method, u64 occurrence = 0) {
  FileContents contents = OsFileRead(arena, path);
  CHECK(contents.ok);

  String8List lines = Str8SplitChar(arena, contents.data, '\n');
  u64 current = 0;
  for (String8Node *node = lines.first; node != nullptr; node = node->next) {
    if (node->string.size == 0) continue;
    JsonValue *root = ParseJsonOrFail(arena, node->string);
    String8 actual = {};
    CHECK(JsonGetString(JsonObjectGet(root, Str8Lit("method")), &actual));
    if (!Str8Match(actual, method)) {
      current += 1;
      continue;
    }
    if (occurrence == 0) return current;
    occurrence -= 1;
    current += 1;
  }

  TestFail(__FILE__, __LINE__, "recorded method %.*s missing", (int)method.size, (char *)method.str);
  return 0;
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

String8 WriteRenameScript(Arena *arena, String8 dir, String8 name, String8 record_path,
                          String8 initialize_result, bool expect_rename) {
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

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("textDocument/didOpen")));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("textDocument/prepareRename")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(
      &writer,
      Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"range\":{\"start\":{\"line\":0,"
              "\"character\":4},\"end\":{\"line\":0,\"character\":9}},\"placeholder\":\"value\"}}")));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  if (expect_rename) {
    CHECK(JsonWriteObjectBegin(&writer));
    CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
    CHECK(JsonWriteString(&writer, Str8Lit("textDocument/rename")));
    CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
    CHECK(JsonWriteArrayBegin(&writer));
    CHECK(JsonWriteObjectBegin(&writer));
    CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
    CHECK(JsonWriteString(&writer, Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":null}")));
    CHECK(JsonWriteObjectEnd(&writer));
    CHECK(JsonWriteArrayEnd(&writer));
    CHECK(JsonWriteObjectEnd(&writer));
  }

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("shutdown")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(
      &writer, Str8Lit(expect_rename ? "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":null}"
                                     : "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":null}")));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("exit")));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));
  String8 script = FinishJsonWriterOrFail(&writer);
  JsonWriterDestroy(&writer);
  return WriteTextFile(arena, dir, name, script);
}

String8 WriteDiagnosticsScript(Arena *arena, String8 dir, String8 name, String8 record_path,
                               String8 initialize_result, String8 uri) {
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

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("textDocument/didOpen")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(
      &writer, PushStr8F(arena,
                         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                         "\"params\":{\"uri\":\"%.*s\",\"version\":1,\"diagnostics\":["
                         "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                         "\"end\":{\"line\":0,\"character\":5}},\"severity\":1,"
                         "\"message\":\"broken\"}]}}",
                         (int)uri.size, (char *)uri.str)));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(
      &writer,
      Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\","
              "\"params\":{\"type\":3,\"message\":\"background note\"}}")));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("textDocument/didClose")));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("shutdown")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&writer, Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}")));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("exit")));
  CHECK(JsonWriteObjectEnd(&writer));

  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));
  String8 script = FinishJsonWriterOrFail(&writer);
  JsonWriterDestroy(&writer);
  return WriteTextFile(arena, dir, name, script);
}

Buffer *OpenDetachedFileBuffer(Editor *ed, const char *name, const char *path, const char *text) {
  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::File, Str8C(name));
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  buffer->path = PushStr8Copy(buffer->arena, Str8C(path));
  BufferSetText(ed, buffer, Str8C(text));
  return buffer;
}

Buffer *OpenFileBuffer(Editor *ed, String8 path) {
  BufferHandle handle = EditorOpenFile(ed, path);
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  return buffer;
}

View *FocusBuffer(Editor *ed, Buffer *buffer, u64 cursor) {
  View *view = EditorFocusedView(ed);
  CHECK(view != nullptr);
  view->buffer = buffer->handle;
  ViewSetCursor(view, buffer, cursor);
  return view;
}

CommandId Lookup(Keymap *map, const char *spec) {
  return KeymapLookupSequence(map, Str8C(spec)).command;
}

}  // namespace

TEST(lsp_wiring_keybindings_match_requested_chords) {
  ArenaScope scope;
  EditorScope editor(scope.arena);

  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "<leader>cf"), (u32)CommandId::lsp_format);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "gi"), (u32)CommandId::lsp_implementation);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "gd"), (u32)CommandId::lsp_definition);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "gD"), (u32)CommandId::lsp_declaration);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "gt"), (u32)CommandId::lsp_type_definition);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "<leader>rn"), (u32)CommandId::lsp_rename);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "<leader>d"), (u32)CommandId::lsp_diagnostic_float);
  CHECK_EQ((u32)Lookup(editor.ed.normal_map, "<C-Space>"), (u32)CommandId::lsp_hover);
  CHECK_EQ((u32)Lookup(editor.ed.insert_map, "<C-Space>"), (u32)CommandId::lsp_completion);
}

TEST(lsp_wiring_commands_report_unavailable_without_lsp) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  Buffer *buffer = OpenDetachedFileBuffer(&editor.ed, "demo.cpp", "/tmp/demo.cpp", "int value;\n");
  FocusBuffer(&editor.ed, buffer, 4);

  CommandId ids[] = {
      CommandId::lsp_format,         CommandId::lsp_implementation, CommandId::lsp_definition,
      CommandId::lsp_declaration,    CommandId::lsp_type_definition, CommandId::lsp_rename,
      CommandId::lsp_completion,     CommandId::lsp_hover,           CommandId::lsp_diagnostic_float,
  };
  for (CommandId id : ids) {
    EditorSetStatus(&editor.ed, Str8Lit(""));
    CommandExec(&editor.ed, id);
    CHECK(editor.ed.status_message.size > 0);
  }
  CHECK(!editor.ed.command_line_active);
}

TEST(lsp_wiring_rename_prompt_prefills_and_cancels_without_request) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_cancel");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 capabilities =
      Str8Lit("{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":{\"openClose\":true,"
              "\"change\":2},\"renameProvider\":{\"prepareProvider\":true}}");
  String8 initialize_result = MakeInitializeResult(scope.arena, capabilities);
  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("rename_cancel.jsonl"));
  String8 script = WriteRenameScript(scope.arena, fixture.path, Str8Lit("rename_cancel_script.json"),
                                     record, initialize_result, false);

  ResolveState resolve = {};
  resolve.entries.push_back({.root = std::string((const char *)root.str, (size_t)root.size),
                             .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root, Str8Lit("rename.cpp"),
                                               Str8Lit("int value = 1;\n")));
  View *view = FocusBuffer(&editor.ed, buffer, 4);
  (void)view;
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    EditorLspBufferInfo info = {};
    return EditorLspGetBufferInfo(&editor.ed, buffer, &info) && info.did_open_sent;
  }));

  CommandExec(&editor.ed, CommandId::lsp_rename);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return editor.ed.command_line_active;
  }));

  Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
  CHECK(command != nullptr);
  CHECK_STR(BufferTextAll(scope.arena, command), Str8Lit("value"));
  CHECK(editor.ed.command_line_active);
  CHECK_EQ((u32)editor.ed.command_view->vim.mode, (u32)VimMode::Insert);
  CHECK_STR(editor.ed.command_line_purpose, Str8Lit("rename"));

  EditorProcessSpec(&editor.ed, "<Esc><Esc>");
  CHECK(!editor.ed.command_line_active);
  CHECK_EQ(FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/prepareRename")), (u64)3);
  CHECK_EQ(RecordedMessageCount(record), (u64)4);
}

TEST(lsp_wiring_rename_prompt_empty_submit_reports_status) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_empty");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 capabilities =
      Str8Lit("{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":{\"openClose\":true,"
              "\"change\":2},\"renameProvider\":{\"prepareProvider\":true}}");
  String8 initialize_result = MakeInitializeResult(scope.arena, capabilities);
  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("rename_empty.jsonl"));
  String8 script = WriteRenameScript(scope.arena, fixture.path, Str8Lit("rename_empty_script.json"),
                                     record, initialize_result, false);

  ResolveState resolve = {};
  resolve.entries.push_back({.root = std::string((const char *)root.str, (size_t)root.size),
                             .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root, Str8Lit("rename.cpp"),
                                               Str8Lit("int value = 1;\n")));
  FocusBuffer(&editor.ed, buffer, 4);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    EditorLspBufferInfo info = {};
    return EditorLspGetBufferInfo(&editor.ed, buffer, &info) && info.did_open_sent;
  }));

  CommandExec(&editor.ed, CommandId::lsp_rename);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return editor.ed.command_line_active;
  }));

  EditorProcessSpec(&editor.ed, "<C-w><CR>");
  CHECK(!editor.ed.command_line_active);
  CHECK(StatusContains(&editor.ed, "rename needs a name"));
  CHECK_EQ(RecordedMessageCount(record), (u64)4);
}

TEST(lsp_wiring_rename_prompt_submits_new_name) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "rename_submit");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 capabilities =
      Str8Lit("{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":{\"openClose\":true,"
              "\"change\":2},\"renameProvider\":{\"prepareProvider\":true}}");
  String8 initialize_result = MakeInitializeResult(scope.arena, capabilities);
  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("rename_submit.jsonl"));
  String8 script = WriteRenameScript(scope.arena, fixture.path, Str8Lit("rename_submit_script.json"),
                                     record, initialize_result, true);

  ResolveState resolve = {};
  resolve.entries.push_back({.root = std::string((const char *)root.str, (size_t)root.size),
                             .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root, Str8Lit("rename.cpp"),
                                               Str8Lit("int value = 1;\n")));
  FocusBuffer(&editor.ed, buffer, 4);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    EditorLspBufferInfo info = {};
    return EditorLspGetBufferInfo(&editor.ed, buffer, &info) && info.did_open_sent;
  }));

  CommandExec(&editor.ed, CommandId::lsp_rename);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return editor.ed.command_line_active;
  }));

  EditorProcessSpec(&editor.ed, "<C-w>renamed<CR>");
  CHECK(!editor.ed.command_line_active);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return RecordedMessageCount(record) >= 5;
  }));

  JsonValue *rename =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/rename")));
  String8 new_name = {};
  CHECK(JsonGetString(JsonObjectGet(JsonObjectGet(rename, Str8Lit("params")), Str8Lit("newName")),
                       &new_name));
  CHECK_STR(new_name, Str8Lit("renamed"));
}

TEST(lsp_wiring_routes_publish_diagnostics_and_log_messages) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "notifications");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 file_path = WriteTextFile(scope.arena, root, Str8Lit("note.cpp"), Str8Lit("value\n"));
  String8 uri = LspPathToUri(scope.arena, file_path);
  String8 capabilities =
      Str8Lit("{\"positionEncoding\":\"utf-16\",\"textDocumentSync\":{\"openClose\":true,"
              "\"change\":2,\"save\":{}},\"publishDiagnostics\":{}}");
  String8 initialize_result = MakeInitializeResult(scope.arena, capabilities);
  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("notifications.jsonl"));
  String8 script =
      WriteDiagnosticsScript(scope.arena, fixture.path, Str8Lit("notifications_script.json"), record,
                             initialize_result, uri);

  ResolveState resolve = {};
  resolve.entries.push_back({.root = std::string((const char *)root.str, (size_t)root.size),
                             .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer = OpenFileBuffer(&editor.ed, file_path);
  FocusBuffer(&editor.ed, buffer, 0);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    u64 count = 0;
    return editor.ed.lsp_ui != nullptr &&
           EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &count) != nullptr && count == 1 &&
           StatusContains(&editor.ed, "background note");
  }));

  u64 count = 0;
  const LspDiagnostic *diagnostics = EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &count);
  CHECK_EQ(count, (u64)1);
  CHECK(diagnostics != nullptr);
  CHECK_STR(diagnostics[0].message, Str8Lit("broken"));

  BufferClose(&editor.ed.buffers, &editor.ed, buffer->handle);
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 4; }));
  diagnostics = EditorLspUiDiagnosticsForUri(editor.ed.lsp_ui, uri, &count);
  CHECK(diagnostics == nullptr);
  CHECK_EQ(count, (u64)0);
}

TEST(lsp_wiring_popup_input_has_priority_and_other_chords_continue) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  Buffer *buffer = OpenDetachedFileBuffer(&editor.ed, "popup.cpp", "/tmp/popup.cpp", "abc");
  View *view = FocusBuffer(&editor.ed, buffer, BufferSize(buffer));
  view->vim.mode = VimMode::Insert;

  String8 error = {};
  CHECK(EditorLspUiShowCompletionResult(
      editor.ed.lsp_ui, buffer, LspPositionEncoding::Utf8, view->cursor, (i64)buffer->edit_serial,
      ParseJsonOrFail(scope.arena, Str8Lit("[{\"label\":\"one\"},{\"label\":\"two\"}]")), &error));

  EditorProcessChord(&editor.ed, KeyChordKey(Key::Down));
  CHECK_EQ(EditorLspUiPopup(editor.ed.lsp_ui)->completion.selected, (u64)1);
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("abc"));

  EditorProcessChord(&editor.ed, KeyChordChar('x'));
  CHECK_EQ((u64)EditorLspUiPopup(editor.ed.lsp_ui)->kind, (u64)EditorLspUiPopupKind::None);
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("abcx"));
}

TEST(lsp_wiring_popup_invalidates_on_switch_and_edit) {
  ArenaScope scope;
  EditorScope editor(scope.arena);
  Buffer *first = OpenDetachedFileBuffer(&editor.ed, "first.cpp", "/tmp/first.cpp", "abc");
  Buffer *second = OpenDetachedFileBuffer(&editor.ed, "second.cpp", "/tmp/second.cpp", "def");

  String8 error = {};
  CHECK(EditorLspUiShowHoverResult(editor.ed.lsp_ui, first, LspPositionEncoding::Utf8, 0,
                                   (i64)first->edit_serial,
                                   ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":\"hover\"}")),
                                   &error));
  CHECK_EQ((u64)EditorLspUiPopup(editor.ed.lsp_ui)->kind, (u64)EditorLspUiPopupKind::Hover);

  EditorShowBuffer(&editor.ed, second->handle);
  CHECK_EQ((u64)EditorLspUiPopup(editor.ed.lsp_ui)->kind, (u64)EditorLspUiPopupKind::None);

  CHECK(EditorLspUiShowHoverResult(editor.ed.lsp_ui, second, LspPositionEncoding::Utf8, 0,
                                   (i64)second->edit_serial,
                                   ParseJsonOrFail(scope.arena, Str8Lit("{\"contents\":\"hover\"}")),
                                   &error));
  BufferInsert(&editor.ed, second, BufferSize(second), Str8Lit("!"), BufferSize(second),
               BufferSize(second) + 1);
  CHECK_EQ((u64)EditorLspUiPopup(editor.ed.lsp_ui)->kind, (u64)EditorLspUiPopupKind::None);
}
