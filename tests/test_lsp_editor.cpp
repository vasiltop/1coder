#include "editor/buffer_registry.h"
#include "editor/editor.h"
#include "editor/lsp.h"
#include "lsp/json.h"
#include "os/os_file.h"
#include "test.h"

#include <chrono>
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
    path = PushStr8F(arena, "%.*s/build/test_lsp_editor_%s_%d", (int)cwd.size, (char *)cwd.str,
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
  bool missing = false;
};

struct ResolveState {
  std::vector<ResolveEntry> entries;
  u64 call_count = 0;
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
  if (command) *command = {};
  if (state == nullptr || command == nullptr) return false;
  state->call_count += 1;

  std::string root_key((const char *)root.str, (size_t)root.size);
  for (const ResolveEntry &entry : state->entries) {
    if (entry.root != root_key) continue;
    if (entry.missing) return false;

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

String8 MakeInitializeResult(Arena *arena, LspPositionEncoding encoding, bool save_include_text = false) {
  const char *encoding_name = "utf-16";
  switch (encoding) {
    case LspPositionEncoding::Utf8: encoding_name = "utf-8"; break;
    case LspPositionEncoding::Utf16: encoding_name = "utf-16"; break;
    case LspPositionEncoding::Utf32: encoding_name = "utf-32"; break;
  }

  return PushStr8F(
      arena,
      "{\"capabilities\":{\"positionEncoding\":\"%s\",\"textDocumentSync\":{\"openClose\":true,"
      "\"change\":2,\"save\":{\"includeText\":%s}}}}",
      encoding_name, save_include_text ? "true" : "false");
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

Buffer *OpenFileBuffer(Editor *ed, String8 path) {
  BufferHandle handle = EditorOpenFile(ed, path);
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  return buffer;
}

bool BufferDidOpen(Editor *ed, Buffer *buffer) {
  EditorLspBufferInfo info = {};
  if (!EditorLspGetBufferInfo(ed, buffer, &info)) return false;
  return info.did_open_sent;
}

TEST(lsp_editor_disabled_by_default_and_non_file_buffers_do_not_attach) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "disabled_nonfile");
  EditorScope editor(scope.arena);

  Buffer *disabled_file =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, fixture.path, Str8Lit("note.cpp"),
                                               Str8Lit("int x = 1;\n")));

  EditorLspBufferInfo info = {};
  CHECK(!EditorLspGetBufferInfo(&editor.ed, disabled_file, &info));
  BufferClose(&editor.ed.buffers, &editor.ed, disabled_file->handle);

  String8 explorer_dir = OsPathJoin(scope.arena, fixture.path, Str8Lit("dir"));
  CHECK(OsMakeDirs(explorer_dir));
  String8 live_root = OsPathJoin(scope.arena, fixture.path, Str8Lit("live"));
  CHECK(OsMakeDirs(live_root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, live_root, Str8Lit(".git")), String8{}));

  ResolveState resolve = {};
  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("file.jsonl"));
  String8 script_path =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("file.json"), record_path,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("shutdown"), Str8Lit("exit")});
  resolve.entries.push_back(
      {.root = std::string((const char *)live_root.str, (size_t)live_root.size),
       .script_path = std::string((const char *)script_path.str, (size_t)script_path.size)});

  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);
  Buffer *file =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, live_root, Str8Lit("live.cpp"),
                                               Str8Lit("int live = 1;\n")));
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, file);
  }));

  Buffer *scratch = EditorFocusedBuffer(&editor.ed);
  CHECK(scratch != nullptr);
  CHECK(scratch->kind == BufferKind::Scratch);
  CHECK(!EditorLspGetBufferInfo(&editor.ed, scratch, &info));

  Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
  CHECK(command != nullptr);
  CHECK(command->kind == BufferKind::Command);
  CHECK(!EditorLspGetBufferInfo(&editor.ed, command, &info));

  BufferHandle explorer_handle = EditorOpenFile(&editor.ed, explorer_dir);
  CHECK(explorer_handle.index != 0);
  Buffer *explorer = BufferFromHandle(&editor.ed.buffers, explorer_handle);
  CHECK(explorer != nullptr);
  CHECK(explorer->kind == BufferKind::Explorer);
  CHECK(!EditorLspGetBufferInfo(&editor.ed, explorer, &info));
}

TEST(lsp_editor_reuses_session_per_root_and_separates_roots) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "sessions");
  EditorScope editor(scope.arena);

  String8 root_one = OsPathJoin(scope.arena, fixture.path, Str8Lit("one"));
  String8 root_two = OsPathJoin(scope.arena, fixture.path, Str8Lit("two"));
  CHECK(OsMakeDirs(root_one));
  CHECK(OsMakeDirs(root_two));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root_one, Str8Lit(".git")), String8{}));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root_two, Str8Lit(".git")), String8{}));

  String8 record_one = OsPathJoin(scope.arena, fixture.path, Str8Lit("one.jsonl"));
  String8 record_two = OsPathJoin(scope.arena, fixture.path, Str8Lit("two.jsonl"));
  String8 script_one =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("one.json"), record_one,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/didOpen"),
                          Str8Lit("shutdown"), Str8Lit("exit")});
  String8 script_two =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("two.json"), record_two,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root_one.str, (size_t)root_one.size),
       .script_path = std::string((const char *)script_one.str, (size_t)script_one.size)});
  resolve.entries.push_back(
      {.root = std::string((const char *)root_two.str, (size_t)root_two.size),
       .script_path = std::string((const char *)script_two.str, (size_t)script_two.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *alpha =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root_one, Str8Lit("alpha.cpp"), Str8Lit("int a;\n")));
  Buffer *beta =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root_one, Str8Lit("beta.cpp"), Str8Lit("int b;\n")));
  Buffer *gamma =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root_two, Str8Lit("gamma.cpp"), Str8Lit("int c;\n")));

  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, alpha) && BufferDidOpen(&editor.ed, beta) &&
           BufferDidOpen(&editor.ed, gamma);
  }));

  EditorLspBufferInfo alpha_info = {};
  EditorLspBufferInfo beta_info = {};
  EditorLspBufferInfo gamma_info = {};
  CHECK(EditorLspGetBufferInfo(&editor.ed, alpha, &alpha_info));
  CHECK(EditorLspGetBufferInfo(&editor.ed, beta, &beta_info));
  CHECK(EditorLspGetBufferInfo(&editor.ed, gamma, &gamma_info));
  CHECK(alpha_info.session == beta_info.session);
  CHECK(alpha_info.client == beta_info.client);
  CHECK(alpha_info.session != gamma_info.session);
  CHECK(alpha_info.client != gamma_info.client);

  CHECK(WaitUntil([&]() {
    return RecordedMessageCount(record_one) == 4 && RecordedMessageCount(record_two) == 3;
  }));
  CHECK_EQ(FindRecordedMethod(scope.arena, record_one, Str8Lit("initialize")), (u64)0);
  CHECK_EQ(FindRecordedMethod(scope.arena, record_two, Str8Lit("initialize")), (u64)0);
  CHECK_EQ(RecordedMessageCount(record_one), (u64)4);
  CHECK_EQ(RecordedMessageCount(record_two), (u64)3);
}

TEST(lsp_editor_defers_open_until_initialize_and_uses_latest_text) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "pending_open");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("pending.jsonl"));
  String8 initialize_result = MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16);

  JsonWriter writer = {};
  JsonWriterInit(&writer, scope.arena);
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("recordPath")));
  CHECK(JsonWriteString(&writer, record));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("onMessage")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&writer, Str8Lit("initialize")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sleepMs")));
  CHECK(JsonWriteU64(&writer, 60));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&writer,
                        PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":%.*s}",
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
  String8 script_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("pending.json"), script);

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script_path.str, (size_t)script_path.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root, Str8Lit("main.cpp"), Str8Lit("int a;\n")));
  CHECK(!BufferDidOpen(&editor.ed, buffer));

  BufferReplace(&editor.ed, buffer, RangeU64{4, 5}, Str8Lit("value"), 4, 9);
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer);
  }));

  EditorLspBufferInfo info = {};
  CHECK(EditorLspGetBufferInfo(&editor.ed, buffer, &info));
  CHECK_EQ(info.version, (i64)2);

  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 3; }));
  u64 did_open_index = FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didOpen"));
  JsonValue *did_open = RecordedMessageAt(scope.arena, record, did_open_index);
  JsonValue *params = JsonObjectGet(did_open, Str8Lit("params"));
  JsonValue *document = JsonObjectGet(params, Str8Lit("textDocument"));
  String8 text = {};
  i64 version = 0;
  CHECK(JsonGetString(JsonObjectGet(document, Str8Lit("text")), &text));
  CHECK(JsonGetI64(JsonObjectGet(document, Str8Lit("version")), &version));
  CHECK_STR(text, Str8Lit("int value;\n"));
  CHECK_EQ(version, (i64)2);
  CHECK_EQ(RecordedMessageCount(record), (u64)3);
}

TEST(lsp_editor_sends_incremental_change_with_pre_edit_utf8_range_and_version) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "incremental");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("incremental.jsonl"));
  String8 script =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("incremental_script.json"), record,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf8),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/didChange"),
                          Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer = OpenFileBuffer(&editor.ed,
                                  WriteTextFile(scope.arena, root, Str8Lit("utf8.cpp"),
                                                Str8Lit("a\xC3\xA9\xF0\x9F\x99\x82" "b\n")));
  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer);
  }));

  BufferReplace(&editor.ed, buffer, RangeU64{1, 7}, Str8Lit("Z"), 1, 2);
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 4; }));

  JsonValue *did_change =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didChange")));
  JsonValue *params = JsonObjectGet(did_change, Str8Lit("params"));
  JsonValue *document = JsonObjectGet(params, Str8Lit("textDocument"));
  i64 version = 0;
  CHECK(JsonGetI64(JsonObjectGet(document, Str8Lit("version")), &version));
  CHECK_EQ(version, (i64)2);

  JsonValue *changes = JsonObjectGet(params, Str8Lit("contentChanges"));
  CHECK_EQ(JsonArrayCount(changes), (u64)1);
  JsonValue *change = JsonArrayItem(changes, 0);
  String8 text = {};
  CHECK(JsonGetString(JsonObjectGet(change, Str8Lit("text")), &text));
  CHECK_STR(text, Str8Lit("Z"));

  JsonValue *range = JsonObjectGet(change, Str8Lit("range"));
  u64 start_line = 99, start_char = 99, end_line = 99, end_char = 99;
  CHECK(JsonGetU64(JsonObjectGet(JsonObjectGet(range, Str8Lit("start")), Str8Lit("line")), &start_line));
  CHECK(JsonGetU64(JsonObjectGet(JsonObjectGet(range, Str8Lit("start")), Str8Lit("character")),
                   &start_char));
  CHECK(JsonGetU64(JsonObjectGet(JsonObjectGet(range, Str8Lit("end")), Str8Lit("line")), &end_line));
  CHECK(JsonGetU64(JsonObjectGet(JsonObjectGet(range, Str8Lit("end")), Str8Lit("character")), &end_char));
  CHECK_EQ(start_line, (u64)0);
  CHECK_EQ(start_char, (u64)1);
  CHECK_EQ(end_line, (u64)0);
  CHECK_EQ(end_char, (u64)7);
}

TEST(lsp_editor_full_reset_save_close_and_shutdown_notifications) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "reset_save_close");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("reset.jsonl"));
  String8 script =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("reset_script.json"), record,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16, true),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/didChange"),
                          Str8Lit("textDocument/didSave"), Str8Lit("textDocument/didClose"),
                          Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  String8 file_path = WriteTextFile(scope.arena, root, Str8Lit("save.cpp"), Str8Lit("one\n"));
  BufferHandle handle = EditorOpenFile(&editor.ed, file_path);
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&editor.ed.buffers, handle);
  CHECK(buffer != nullptr);
  EditorShowBuffer(&editor.ed, handle);

  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer);
  }));

  BufferSetText(&editor.ed, buffer, Str8Lit("reset text\n"));
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 4; }));

  JsonValue *did_change =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didChange")));
  JsonValue *changes = JsonObjectGet(JsonObjectGet(did_change, Str8Lit("params")), Str8Lit("contentChanges"));
  CHECK_EQ(JsonArrayCount(changes), (u64)1);
  JsonValue *change = JsonArrayItem(changes, 0);
  CHECK(JsonObjectGet(change, Str8Lit("range")) == nullptr);
  String8 reset_text = {};
  CHECK(JsonGetString(JsonObjectGet(change, Str8Lit("text")), &reset_text));
  CHECK_STR(reset_text, Str8Lit("reset text\n"));

  EditorProcessSpec(&editor.ed, ":w<CR>");
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 5; }));
  JsonValue *did_save =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didSave")));
  String8 saved_text = {};
  CHECK(JsonGetString(JsonObjectGet(JsonObjectGet(did_save, Str8Lit("params")), Str8Lit("text")),
                      &saved_text));
  CHECK_STR(saved_text, Str8Lit("reset text\n"));

  BufferClose(&editor.ed.buffers, &editor.ed, handle);
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 6; }));
  JsonValue *did_close =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didClose")));
  String8 closed_uri = {};
  CHECK(JsonGetString(JsonObjectGet(JsonObjectGet(JsonObjectGet(did_close, Str8Lit("params")),
                                                  Str8Lit("textDocument")),
                                    Str8Lit("uri")),
                      &closed_uri));
  CHECK_STR(closed_uri, LspPathToUri(scope.arena, file_path));
}

TEST(lsp_editor_missing_server_is_nonfatal) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "missing_server");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size), .script_path = {}, .missing = true});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer =
      OpenFileBuffer(&editor.ed, WriteTextFile(scope.arena, root, Str8Lit("miss.cpp"), Str8Lit("int x;\n")));
  CHECK(!EditorLspGetBufferInfo(&editor.ed, buffer, nullptr));
  CHECK(editor.ed.status_message.size > 0);

  BufferReplace(&editor.ed, buffer, RangeU64{4, 5}, Str8Lit("value"), 4, 9);
  BufferReplace(&editor.ed, buffer, RangeU64{4, 9}, Str8Lit("name"), 4, 8);
  String8 text = BufferTextAll(scope.arena, buffer);
  CHECK_STR(text, Str8Lit("int name;"));
  CHECK_EQ(resolve.call_count, (u64)1);
}

TEST(lsp_editor_save_as_rebinds_the_document_uri) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "save_as");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  CHECK(OsMakeDirs(root));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));

  String8 record = OsPathJoin(scope.arena, fixture.path, Str8Lit("save_as.jsonl"));
  String8 script =
      WriteSessionScript(scope.arena, fixture.path, Str8Lit("save_as_script.json"), record,
                         MakeInitializeResult(scope.arena, LspPositionEncoding::Utf16, true),
                         {Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/didClose"),
                          Str8Lit("textDocument/didOpen"), Str8Lit("textDocument/didSave"),
                          Str8Lit("shutdown"), Str8Lit("exit")});

  ResolveState resolve = {};
  resolve.entries.push_back(
      {.root = std::string((const char *)root.str, (size_t)root.size),
       .script_path = std::string((const char *)script.str, (size_t)script.size)});
  EditorLspConfig config = MakeLspConfig(&resolve);
  EditorLspEnable(&editor.ed, &config);

  String8 old_path = WriteTextFile(scope.arena, root, Str8Lit("before.cpp"), Str8Lit("int value = 1;\n"));
  String8 new_path = OsPathJoin(scope.arena, root, Str8Lit("after.cpp"));
  BufferHandle handle = EditorOpenFile(&editor.ed, old_path);
  CHECK(handle.index != 0);
  Buffer *buffer = BufferFromHandle(&editor.ed.buffers, handle);
  CHECK(buffer != nullptr);
  EditorShowBuffer(&editor.ed, handle);

  CHECK(WaitUntil([&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer);
  }));

  EditorProcessSpec(&editor.ed, PushStr8F(scope.arena, ":w %.*s<CR>", (int)new_path.size,
                                          (char *)new_path.str));
  CHECK(WaitUntil([&]() { return RecordedMessageCount(record) >= 6; }));

  JsonValue *did_close =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didClose")));
  String8 closed_uri = {};
  CHECK(JsonGetString(JsonObjectGet(JsonObjectGet(JsonObjectGet(did_close, Str8Lit("params")),
                                                  Str8Lit("textDocument")),
                                    Str8Lit("uri")),
                      &closed_uri));
  CHECK_STR(closed_uri, LspPathToUri(scope.arena, old_path));

  JsonValue *did_open =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didOpen"), 1));
  JsonValue *opened_document = JsonObjectGet(JsonObjectGet(did_open, Str8Lit("params")),
                                             Str8Lit("textDocument"));
  String8 opened_uri = {};
  CHECK(JsonGetString(JsonObjectGet(opened_document, Str8Lit("uri")), &opened_uri));
  CHECK_STR(opened_uri, LspPathToUri(scope.arena, new_path));

  JsonValue *did_save =
      RecordedMessageAt(scope.arena, record, FindRecordedMethod(scope.arena, record, Str8Lit("textDocument/didSave")));
  JsonValue *saved_document =
      JsonObjectGet(JsonObjectGet(did_save, Str8Lit("params")), Str8Lit("textDocument"));
  String8 saved_uri = {};
  CHECK(JsonGetString(JsonObjectGet(saved_document, Str8Lit("uri")), &saved_uri));
  CHECK_STR(saved_uri, LspPathToUri(scope.arena, new_path));
}

}  // namespace
