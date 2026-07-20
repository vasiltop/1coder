#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "lsp/client.h"
#include "lsp/json.h"
#include "lsp/protocol.h"
#include "lsp/transport.h"
#include "os/os_file.h"
#include "os/os_process.h"
#include "test.h"

#include <atomic>
#include <chrono>
#include <deque>
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
    path = PushStr8F(arena, "%.*s/build/test_lsp_client_%s_%d", (int)cwd.size, (char *)cwd.str, tag,
                     (int)serial);
    (void)OsDirDeleteRecursive(path);
    CHECK(OsMakeDirs(path));
  }

  ~ScopedFixtureDir() { (void)OsDirDeleteRecursive(path); }
};

struct WakeCounter {
  std::atomic<u32> count = 0;
};

struct NotificationRecord {
  std::string method;
  std::string uri;
  std::string message;
  u64 count = 0;
};

struct ResponseRecord {
  u32 call_count = 0;
  std::thread::id thread_id = {};
  u64 id = 0;
  std::string method;
  u64 document_id = 0;
  u64 buffer_id = 0;
  i64 document_version = 0;
  u32 request_kind = 0;
  void *request_token = nullptr;
  bool cancelled = false;
  bool has_result = false;
  std::string result_value;
  bool has_error = false;
  i64 error_code = 0;
  std::string error_message;
};

struct ApplyEditState {
  u32 call_count = 0;
  bool next_applied = true;
  std::string failure_reason;
  std::string last_label;
};

struct DestroyClientState {
  LspClient *client = nullptr;
  u32 call_count = 0;
};

struct CallbackState {
  std::vector<NotificationRecord> notifications;
  ApplyEditState apply_edit = {};
};

struct MockInbound {
  std::string text;
  bool stderr_message = false;
};

struct MockTransport {
  LspTransportConfig config = {};
  std::vector<std::string> sent;
  std::deque<MockInbound> inbox;
  std::string failure_reason;
  std::string stderr_summary;
  bool started = false;
  bool stopped = false;
  bool auto_shutdown_response = false;
  bool fail_next_send = false;

  static const LspClientTransportHooks *Hooks();

  void PushJson(String8 json, bool stderr_message = false) {
    inbox.push_back({Copy(json), stderr_message});
    Wake();
  }

  void MarkFailed(String8 reason, String8 stderr = {}) {
    failure_reason = Copy(reason);
    if (stderr.size > 0) stderr_summary = Copy(stderr);
    Wake();
  }

  void ClearFailure() {
    failure_reason.clear();
    stderr_summary.clear();
  }

  void ResetForRestart() {
    sent.clear();
    inbox.clear();
    ClearFailure();
    started = false;
    stopped = false;
    fail_next_send = false;
  }

  static std::string Copy(String8 s) { return std::string((const char *)s.str, (size_t)s.size); }

  void Wake() const {
    if (config.wake) config.wake(config.wake_user_data);
  }
};

bool MockTransportStart(void *user_data, const LspTransportConfig *config) {
  MockTransport *transport = (MockTransport *)user_data;
  transport->config = *config;
  transport->started = true;
  transport->stopped = false;
  return true;
}

bool MockTransportSend(void *user_data, String8 json) {
  MockTransport *transport = (MockTransport *)user_data;
  if (transport->fail_next_send) {
    transport->fail_next_send = false;
    return false;
  }
  transport->sent.push_back(MockTransport::Copy(json));

  ArenaScope scope;
  JsonParseResult parsed = JsonParse(scope.arena, json);
  if (transport->auto_shutdown_response && parsed.root != nullptr) {
    JsonValue *method = JsonObjectGet(parsed.root, Str8Lit("method"));
    String8 method_name = {};
    if (JsonGetString(method, &method_name) && Str8Match(method_name, Str8Lit("shutdown"))) {
      JsonValue *id = JsonObjectGet(parsed.root, Str8Lit("id"));
      u64 request_id = 0;
      CHECK(JsonGetU64(id, &request_id));
      transport->PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":null}",
                                    (unsigned long long)request_id));
    }
  }

  return true;
}

bool MockTransportPop(void *user_data, Arena *arena, LspInboundMessage *message) {
  MockTransport *transport = (MockTransport *)user_data;
  if (transport->inbox.empty()) return false;

  MockInbound inbound = transport->inbox.front();
  transport->inbox.pop_front();
  message->json = PushStr8Copy(arena, Str8C(inbound.text.c_str()));
  message->stderr_message = inbound.stderr_message;
  return true;
}

void MockTransportStop(void *user_data) {
  MockTransport *transport = (MockTransport *)user_data;
  transport->stopped = true;
}

bool MockTransportFailed(void *user_data) {
  MockTransport *transport = (MockTransport *)user_data;
  return !transport->failure_reason.empty();
}

String8 MockTransportFailureReason(void *user_data) {
  MockTransport *transport = (MockTransport *)user_data;
  return Str8C(transport->failure_reason.c_str());
}

String8 MockTransportStderrSummary(void *user_data) {
  MockTransport *transport = (MockTransport *)user_data;
  return Str8C(transport->stderr_summary.c_str());
}

const LspClientTransportHooks *MockTransport::Hooks() {
  static const LspClientTransportHooks hooks = {
      MockTransportStart,
      MockTransportSend,
      MockTransportPop,
      MockTransportStop,
      MockTransportFailed,
      MockTransportFailureReason,
      MockTransportStderrSummary,
  };
  return &hooks;
}

void WakeCounterProc(void *user_data) {
  WakeCounter *counter = (WakeCounter *)user_data;
  counter->count.fetch_add(1, std::memory_order_relaxed);
}

JsonValue *ParseJsonOrFail(Arena *arena, String8 text) {
  JsonParseResult result = JsonParse(arena, text);
  if (result.root == nullptr) {
    TestFail(__FILE__, __LINE__, "parse failed at %llu: %.*s", (unsigned long long)result.error_offset,
             (int)result.error.size, (char *)result.error.str);
  }
  return result.root;
}

std::string CopyString(String8 s) { return std::string((const char *)s.str, (size_t)s.size); }

template <typename Fn>
bool WaitUntil(Fn predicate, i32 timeout_ms = 750) {
  Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

String8 WriteTextFile(Arena *arena, String8 dir, String8 name, String8 text) {
  String8 path = OsPathJoin(arena, dir, name);
  CHECK(OsFileWrite(path, text));
  return path;
}

String8 ReadFileOrFail(Arena *arena, String8 path) {
  FileContents contents = OsFileRead(arena, path);
  CHECK(contents.ok);
  return contents.data;
}

String8 FinishJsonWriterOrFail(JsonWriter *writer) {
  String8 json = JsonWriterFinish(writer);
  if (json.size == 0) {
    String8 error = JsonWriterError(writer);
    TestFail(__FILE__, __LINE__, "json writer failed: %.*s", (int)error.size, (char *)error.str);
  }
  return json;
}

bool FileHasContents(String8 path) {
  Arena *arena = ArenaAlloc(KB(64));
  if (arena == nullptr) return false;
  FileContents contents = OsFileRead(arena, path);
  bool ok = contents.ok && contents.data.size > 0;
  ArenaRelease(arena);
  return ok;
}

String8 RootName(String8 root) {
  String8 base = Str8PathBase(root);
  if (base.size > 0) return base;
  return root;
}

LspServerCommand MakeFakeServerCommand(Arena *arena, String8 root, std::initializer_list<String8> args) {
  String8 *arguments = PushArray(arena, String8, args.size());
  u64 index = 0;
  for (String8 arg : args) {
    arguments[index++] = PushStr8Copy(arena, arg);
  }

  return LspServerCommand{
      LspLanguage::Cpp,
      Str8Lit("cpp"),
      Str8C(EDITOR_FAKE_LSP_PATH),
      arguments,
      (u64)args.size(),
      PushStr8Copy(arena, root),
  };
}

void CheckJsonRpcEnvelope(const JsonValue *root) {
  CHECK(root != nullptr);
  CHECK(root->kind == JsonKind::Object);
  String8 version = {};
  CHECK(JsonGetString(JsonObjectGet(root, Str8Lit("jsonrpc")), &version));
  CHECK_STR(version, Str8Lit("2.0"));
}

u64 JsonIdOrFail(const JsonValue *root) {
  u64 id = 0;
  CHECK(JsonGetU64(JsonObjectGet(root, Str8Lit("id")), &id));
  CHECK(id != 0);
  return id;
}

void ExpectMethod(const JsonValue *root, String8 expected) {
  String8 method = {};
  CHECK(JsonGetString(JsonObjectGet(root, Str8Lit("method")), &method));
  CHECK_STR(method, expected);
}

void CaptureNotification(void *user_data, String8 method, const JsonValue *params) {
  CallbackState *state = (CallbackState *)user_data;
  NotificationRecord record = {};
  record.method = CopyString(method);

  if (params != nullptr && params->kind == JsonKind::Object) {
    String8 uri = {};
    String8 message = {};
    (void)JsonGetString(JsonObjectGet(params, Str8Lit("uri")), &uri);
    (void)JsonGetString(JsonObjectGet(params, Str8Lit("message")), &message);
    record.uri = CopyString(uri);
    record.message = CopyString(message);
    record.count = JsonArrayCount(JsonObjectGet(params, Str8Lit("diagnostics")));
  }

  state->notifications.push_back(record);
}

LspClientApplyEditResult CaptureApplyEdit(void *user_data, const JsonValue *edit) {
  CallbackState *state = (CallbackState *)user_data;
  state->apply_edit.call_count += 1;

  if (edit != nullptr && edit->kind == JsonKind::Object) {
    JsonValue *label = JsonObjectGet(edit, Str8Lit("label"));
    String8 text = {};
    if (JsonGetString(label, &text)) state->apply_edit.last_label = CopyString(text);
  }

  LspClientApplyEditResult result = {};
  result.applied = state->apply_edit.next_applied;
  result.failure_reason = Str8C(state->apply_edit.failure_reason.c_str());
  return result;
}

void CaptureResponse(void *user_data, const LspClientResponse *response) {
  ResponseRecord *record = (ResponseRecord *)user_data;
  record->call_count += 1;
  record->thread_id = std::this_thread::get_id();
  record->id = response->id;
  record->method = CopyString(response->method);
  record->document_id = response->context.document_id;
  record->buffer_id = response->context.buffer_id;
  record->document_version = response->context.document_version;
  record->request_kind = response->context.request_kind;
  record->request_token = response->context.request_token;
  record->cancelled = response->cancelled;
  record->has_result = response->has_result;
  record->has_error = response->has_error;
  record->error_code = response->error_code;
  record->error_message = CopyString(response->error_message);

  record->result_value.clear();
  if (response->result != nullptr) {
    String8 text = {};
    if (JsonGetString(response->result, &text)) {
      record->result_value = CopyString(text);
    } else if (response->result->kind == JsonKind::Object) {
      JsonValue *value = JsonObjectGet(response->result, Str8Lit("value"));
      if (JsonGetString(value, &text)) record->result_value = CopyString(text);
    }
  }
}

void DestroyClientOnResponse(void *user_data, const LspClientResponse *response) {
  (void)response;
  DestroyClientState *state = (DestroyClientState *)user_data;
  state->call_count += 1;
  LspClientDestroy(state->client);
}

LspClientApplyEditResult DestroyDuringApplyEdit(void *user_data, const JsonValue *edit) {
  (void)edit;
  DestroyClientState *state = (DestroyClientState *)user_data;
  state->call_count += 1;
  LspClientApplyEditResult result = {};
  result.applied = true;
  LspClientDestroy(state->client);
  return result;
}

void ConfigureClient(Arena *arena, LspClient *client, MockTransport *transport, CallbackState *callbacks,
                     WakeCounter *wake, String8 root, u32 shutdown_timeout_ms = 60) {
  LspClientCallbacks client_callbacks = {};
  client_callbacks.notification = CaptureNotification;
  client_callbacks.apply_edit = CaptureApplyEdit;
  client_callbacks.user_data = callbacks;

  LspClientConfig config = {};
  config.command = MakeFakeServerCommand(arena, root, {});
  config.workspace_root = PushStr8Copy(arena, root);
  config.wake = WakeCounterProc;
  config.wake_user_data = wake;
  config.callbacks = client_callbacks;
  config.transport_hooks = MockTransport::Hooks();
  config.transport_user_data = transport;
  config.shutdown_timeout_ms = shutdown_timeout_ms;

  CHECK(LspClientConfigure(client, &config));
}

u64 StartReadyClient(Arena *arena, LspClient *client, MockTransport *transport, CallbackState *callbacks,
                     WakeCounter *wake, String8 root, String8 initialize_result) {
  ConfigureClient(arena, client, transport, callbacks, wake, root);
  CHECK(LspClientStart(client));
  CHECK_EQ((u64)LspClientGetState(client), (u64)LspClientState::Initializing);
  CHECK_EQ(transport->sent.size(), (size_t)1);

  ArenaScope scope;
  JsonValue *initialize = ParseJsonOrFail(scope.arena, Str8C(transport->sent[0].c_str()));
  u64 initialize_id = JsonIdOrFail(initialize);

  transport->PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":%.*s}",
                                (unsigned long long)initialize_id, (int)initialize_result.size,
                                (char *)initialize_result.str));
  LspClientTick(client);
  CHECK_EQ((u64)LspClientGetState(client), (u64)LspClientState::Ready);
  return initialize_id;
}

TEST(lsp_client_transport_processes_fragmented_frames_stderr_and_send_framing) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "transport_script");
  WakeCounter wake = {};

  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("received.jsonl"));
  JsonWriter script_writer = {};
  JsonWriterInit(&script_writer, scope.arena);
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("recordPath")));
  CHECK(JsonWriteString(&script_writer, record_path));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("startup")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendStderr")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("boot-1\n")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sleepMs")));
  CHECK(JsonWriteU64(&script_writer, 25));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&script_writer,
                        Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"server/one\",\"params\":{\"seq\":1}}")));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("fragments")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteU64(&script_writer, 9));
  CHECK(JsonWriteU64(&script_writer, 7));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sleepMs")));
  CHECK(JsonWriteU64(&script_writer, 25));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendStderr")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("boot-2\n")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sleepMs")));
  CHECK(JsonWriteU64(&script_writer, 25));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&script_writer,
                        Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"server/two\",\"params\":{\"seq\":2}}")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  String8 script = FinishJsonWriterOrFail(&script_writer);
  JsonWriterDestroy(&script_writer);
  String8 script_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("transport_script.json"), script);

  String8 args[] = {Str8Lit("--script"), script_path};
  LspServerCommand command = MakeFakeServerCommand(scope.arena, fixture.path, {args[0], args[1]});
  LspTransportConfig config = {command, WakeCounterProc, &wake};
  LspTransport transport = {};

  CHECK(LspTransportStart(&transport, &config));

  String8 outbound = Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"client/ping\",\"params\":{\"value\":7}}");
  CHECK(LspTransportSend(&transport, outbound));

  CHECK(WaitUntil([&]() { return wake.count.load(std::memory_order_relaxed) >= 4; }));
  CHECK(WaitUntil([&]() { return FileHasContents(record_path); }));

  std::vector<MockInbound> popped;
  for (;;) {
    LspInboundMessage message = {};
    if (!LspTransportPop(&transport, scope.arena, &message)) break;
    popped.push_back({CopyString(message.json), message.stderr_message});
  }

  CHECK_EQ(popped.size(), (size_t)4);
  CHECK(popped[0].stderr_message);
  CHECK_STR(Str8C(popped[0].text.c_str()), Str8Lit("boot-1\n"));
  CHECK(!popped[1].stderr_message);
  CHECK_STR(Str8C(popped[1].text.c_str()),
            Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"server/one\",\"params\":{\"seq\":1}}"));
  CHECK(popped[2].stderr_message);
  CHECK_STR(Str8C(popped[2].text.c_str()), Str8Lit("boot-2\n"));
  CHECK(!popped[3].stderr_message);
  CHECK_STR(Str8C(popped[3].text.c_str()),
            Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"server/two\",\"params\":{\"seq\":2}}"));

  LspTransportStop(&transport);
  LspTransportStop(&transport);

  String8 lines = ReadFileOrFail(scope.arena, record_path);
  String8List received = Str8SplitChar(scope.arena, lines, '\n');
  CHECK(received.node_count >= 1);
  if (received.first == nullptr) return;
  JsonValue *received_json = ParseJsonOrFail(scope.arena, received.first->string);
  ExpectMethod(received_json, Str8Lit("client/ping"));
}

TEST(lsp_client_transport_reports_queue_overflow_and_caps_stderr_summary) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "transport_overflow");
  WakeCounter wake = {};

  String8 script = Str8Lit(
      "{\"startup\":["
      "{\"generatedStderrBytes\":70000,\"stderrPrefix\":\"prefix-\",\"stderrSuffix\":\"-tail\"},"
      "{\"generatedFrameMethod\":\"overflow\",\"generatedFrameBytes\":1048576,\"generatedFrameCount\":40}"
      "]}");
  String8 script_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("transport_overflow.json"), script);

  LspServerCommand command =
      MakeFakeServerCommand(scope.arena, fixture.path, {Str8Lit("--script"), script_path});
  LspTransportConfig config = {command, WakeCounterProc, &wake};
  LspTransport transport = {};

  CHECK(LspTransportStart(&transport, &config));
  CHECK(WaitUntil([&]() { return LspTransportFailed(&transport); }, 1500));
  CHECK(wake.count.load(std::memory_order_relaxed) > 0);

  String8 reason = LspTransportFailureReason(&transport);
  CHECK(reason.size > 0);
  String8 stderr_summary = LspTransportStderrSummary(&transport);
  CHECK(stderr_summary.size <= KB(64));
  CHECK(Str8FindFirst(stderr_summary, Str8Lit("truncated")) < stderr_summary.size);
  CHECK(Str8EndsWith(stderr_summary, Str8Lit("-tail")));

  LspTransportStop(&transport);
}

TEST(lsp_client_transport_forced_stop_is_bounded_and_idempotent) {
  ArenaScope scope;
  WakeCounter wake = {};

  LspServerCommand command = MakeFakeServerCommand(scope.arena, String8{}, {Str8Lit("--sleep")});
  LspTransportConfig config = {command, WakeCounterProc, &wake};
  LspTransport transport = {};

  CHECK(LspTransportStart(&transport, &config));
  Clock::time_point start = Clock::now();
  LspTransportStop(&transport);
  LspTransportStop(&transport);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  CHECK(elapsed.count() < 750);
}

TEST(lsp_client_initialize_request_and_initialized_lifecycle) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "initialize");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  ConfigureClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path);
  CHECK(LspClientStart(&client));
  CHECK(transport.started);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Initializing);
  CHECK_EQ(transport.sent.size(), (size_t)1);

  JsonValue *initialize = ParseJsonOrFail(scope.arena, Str8C(transport.sent[0].c_str()));
  CheckJsonRpcEnvelope(initialize);
  ExpectMethod(initialize, Str8Lit("initialize"));
  CHECK_EQ(transport.sent.size(), (size_t)1);

  JsonValue *params = JsonObjectGet(initialize, Str8Lit("params"));
  CHECK(params != nullptr && params->kind == JsonKind::Object);
  CHECK(JsonObjectGet(params, Str8Lit("processId"))->kind == JsonKind::Null);

  JsonValue *client_info = JsonObjectGet(params, Str8Lit("clientInfo"));
  String8 client_name = {};
  CHECK(JsonGetString(JsonObjectGet(client_info, Str8Lit("name")), &client_name));
  CHECK_STR(client_name, Str8Lit("1coder"));

  String8 root_uri = {};
  CHECK(JsonGetString(JsonObjectGet(params, Str8Lit("rootUri")), &root_uri));
  CHECK_STR(root_uri, LspPathToUri(scope.arena, fixture.path));

  JsonValue *workspace_folders = JsonObjectGet(params, Str8Lit("workspaceFolders"));
  CHECK_EQ(JsonArrayCount(workspace_folders), (u64)1);
  JsonValue *folder = JsonArrayItem(workspace_folders, 0);
  String8 folder_uri = {};
  String8 folder_name = {};
  CHECK(JsonGetString(JsonObjectGet(folder, Str8Lit("uri")), &folder_uri));
  CHECK(JsonGetString(JsonObjectGet(folder, Str8Lit("name")), &folder_name));
  CHECK_STR(folder_uri, root_uri);
  CHECK_STR(folder_name, RootName(fixture.path));

  JsonValue *capabilities = JsonObjectGet(params, Str8Lit("capabilities"));
  CHECK(capabilities != nullptr && capabilities->kind == JsonKind::Object);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("workspace")), Str8Lit("applyEdit"))->boolean);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("workspace")), Str8Lit("configuration"))->boolean);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("workspace")), Str8Lit("workspaceFolders"))->boolean);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("general")), Str8Lit("positionEncodings"))->kind ==
        JsonKind::Array);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("textDocument")), Str8Lit("completion"))->kind ==
        JsonKind::Object);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("textDocument")), Str8Lit("hover"))->kind ==
        JsonKind::Object);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("textDocument")), Str8Lit("publishDiagnostics"))->kind ==
        JsonKind::Object);
  CHECK(JsonObjectGet(JsonObjectGet(capabilities, Str8Lit("textDocument")), Str8Lit("rename"))->kind ==
        JsonKind::Object);

  u64 initialize_id = JsonIdOrFail(initialize);
  transport.PushJson(
      PushStr8F(scope.arena,
                "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"capabilities\":{\"positionEncoding\":\"utf-8\","
                "\"textDocumentSync\":2,\"completionProvider\":{},\"hoverProvider\":true,"
                "\"definitionProvider\":true,\"workspace\":{\"workspaceEdit\":{\"documentChanges\":true}}}}}",
                (unsigned long long)initialize_id));
  LspClientTick(&client);

  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Ready);
  CHECK_EQ((u64)LspClientGetPositionEncoding(&client), (u64)LspPositionEncoding::Utf8);
  CHECK(LspClientGetCapabilities(&client)->completion_provider);
  CHECK(LspClientGetCapabilities(&client)->hover_provider);
  CHECK(LspClientGetCapabilities(&client)->definition_provider);
  CHECK(LspClientGetCapabilities(&client)->workspace_edit.document_changes);
  CHECK_EQ(transport.sent.size(), (size_t)2);

  JsonValue *initialized = ParseJsonOrFail(scope.arena, Str8C(transport.sent[1].c_str()));
  ExpectMethod(initialized, Str8Lit("initialized"));

  LspClientDestroy(&client);
}

TEST(lsp_client_invalid_initialize_result_fails) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "initialize_invalid");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  ConfigureClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path);
  CHECK(LspClientStart(&client));

  JsonValue *initialize = ParseJsonOrFail(scope.arena, Str8C(transport.sent[0].c_str()));
  u64 initialize_id = JsonIdOrFail(initialize);
  transport.PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"capabilities\":7}}",
                               (unsigned long long)initialize_id));
  LspClientTick(&client);

  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Failed);
  CHECK(LspClientGetFailureReason(&client).size > 0);
  CHECK(LspClientCanRestart(&client));

  LspClientDestroy(&client);
}

TEST(lsp_client_initialize_omits_workspace_edit_without_apply_edit_callback) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "initialize_no_apply_edit");
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  LspClientConfig config = {};
  config.command = MakeFakeServerCommand(scope.arena, fixture.path, {});
  config.workspace_root = PushStr8Copy(scope.arena, fixture.path);
  config.wake = WakeCounterProc;
  config.wake_user_data = &wake;
  config.transport_hooks = MockTransport::Hooks();
  config.transport_user_data = &transport;

  CHECK(LspClientConfigure(&client, &config));
  CHECK(LspClientStart(&client));
  CHECK_EQ(transport.sent.size(), (size_t)1);

  JsonValue *initialize = ParseJsonOrFail(scope.arena, Str8C(transport.sent[0].c_str()));
  JsonValue *workspace = JsonObjectGet(JsonObjectGet(JsonObjectGet(initialize, Str8Lit("params")),
                                                     Str8Lit("capabilities")),
                                       Str8Lit("workspace"));
  CHECK(workspace != nullptr && workspace->kind == JsonKind::Object);
  CHECK(!JsonObjectGet(workspace, Str8Lit("applyEdit"))->boolean);
  CHECK(JsonObjectGet(workspace, Str8Lit("workspaceEdit")) == nullptr);

  LspClientDestroy(&client);
}

TEST(lsp_client_correlates_out_of_order_responses_and_rejects_bad_ids) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "request_order");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"positionEncoding\":\"utf-16\",\"hoverProvider\":true}}"));
  std::thread::id main_thread = std::this_thread::get_id();

  ResponseRecord first = {};
  ResponseRecord second = {};
  LspClientRequestContext context_one = {11, 21, 31, 41, &first};
  LspClientRequestContext context_two = {12, 22, 32, 42, &second};

  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{\"value\":1}"),
                                    &context_one, CaptureResponse, &first),
           (u64)2);
  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{]"), &context_one,
                                    CaptureResponse, &first),
           (u64)0);

  auto write_params = [](JsonWriter *writer, void *user_data) -> bool {
    (void)user_data;
    return JsonWriteObjectBegin(writer) && JsonWriteObjectKey(writer, Str8Lit("value")) &&
           JsonWriteString(writer, Str8Lit("two")) && JsonWriteObjectEnd(writer);
  };
  CHECK_EQ(LspClientSendRequest(&client, Str8Lit("textDocument/definition"), write_params, nullptr,
                                &context_two, CaptureResponse, &second),
           (u64)3);
  CHECK_EQ(transport.sent.size(), (size_t)4);

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"value\":\"second\"}}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":1234,\"message\":\"first failed\"}}"));
  LspClientTick(&client);

  CHECK_EQ(second.call_count, (u32)1);
  CHECK(second.thread_id == main_thread);
  CHECK_EQ(second.id, (u64)3);
  CHECK_EQ(second.document_id, (u64)12);
  CHECK_EQ(second.buffer_id, (u64)22);
  CHECK_EQ(second.document_version, (i64)32);
  CHECK_EQ(second.request_kind, (u32)42);
  CHECK(second.request_token == &second);
  CHECK(second.has_result);
  CHECK_STR(Str8C(second.result_value.c_str()), Str8Lit("second"));

  CHECK_EQ(first.call_count, (u32)1);
  CHECK(first.thread_id == main_thread);
  CHECK_EQ(first.id, (u64)2);
  CHECK_EQ(first.document_id, (u64)11);
  CHECK_EQ(first.buffer_id, (u64)21);
  CHECK_EQ(first.document_version, (i64)31);
  CHECK_EQ(first.request_kind, (u32)41);
  CHECK(first.request_token == &first);
  CHECK(first.has_error);
  CHECK_EQ(first.error_code, (i64)1234);
  CHECK_STR(Str8C(first.error_message.c_str()), Str8Lit("first failed"));

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":999,\"result\":null}"));
  LspClientTick(&client);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Failed);
  CHECK(LspClientGetFailureReason(&client).size > 0);

  LspClientDestroy(&client);
}

TEST(lsp_client_cancellation_emits_cancel_request_and_completes_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "cancel");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord response = {};
  LspClientRequestContext context = {77, 88, 99, 111, &response};
  u64 request_id = LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), &context,
                                            CaptureResponse, &response);
  CHECK_EQ(request_id, (u64)2);
  CHECK(LspClientCancel(&client, request_id));

  JsonValue *cancel = ParseJsonOrFail(scope.arena, Str8C(transport.sent.back().c_str()));
  ExpectMethod(cancel, Str8Lit("$/cancelRequest"));
  JsonValue *params = JsonObjectGet(cancel, Str8Lit("params"));
  CHECK_EQ(JsonIdOrFail(params), request_id);

  LspClientTick(&client);
  CHECK_EQ(response.call_count, (u32)1);
  CHECK(response.cancelled);
  CHECK(!response.has_result);

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"value\":\"late\"}}"));
  LspClientTick(&client);
  CHECK_EQ(response.call_count, (u32)1);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Ready);

  LspClientDestroy(&client);
}

TEST(lsp_client_cancellation_yields_to_already_queued_response) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "cancel_race");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord response = {};
  u64 request_id =
      LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), nullptr, CaptureResponse,
                               &response);
  CHECK_EQ(request_id, (u64)2);

  transport.PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"value\":\"ready\"}}",
                               (unsigned long long)request_id));
  CHECK(LspClientCancel(&client, request_id));
  LspClientTick(&client);

  CHECK_EQ(response.call_count, (u32)1);
  CHECK(!response.cancelled);
  CHECK(response.has_result);
  CHECK(response.result_value == "ready");

  LspClientDestroy(&client);
}

TEST(lsp_client_stop_fails_pending_requests_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "stop_pending");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord response = {};
  LspClientRequestContext context = {5, 6, 7, 8, &response};
  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), &context,
                                    CaptureResponse, &response),
           (u64)2);

  LspClientStop(&client);
  CHECK_EQ(response.call_count, (u32)1);
  CHECK(response.has_error);
  CHECK_EQ(response.error_code, (i64)kLspClientErrorStopped);

  LspClientDestroy(&client);
}

TEST(lsp_client_stop_dispatches_cancelled_requests_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "stop_cancelled");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord response = {};
  LspClientRequestContext context = {17, 18, 19, 20, &response};
  u64 request_id = LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), &context,
                                            CaptureResponse, &response);
  CHECK_EQ(request_id, (u64)2);
  CHECK(LspClientCancel(&client, request_id));

  LspClientStop(&client);
  CHECK_EQ(response.call_count, (u32)1);
  CHECK(response.cancelled);

  LspClientDestroy(&client);
}

TEST(lsp_client_stop_shutdown_send_failure_fails_pending_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "stop_shutdown_fail");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord response = {};
  LspClientRequestContext context = {21, 22, 23, 24, &response};
  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), &context,
                                    CaptureResponse, &response),
           (u64)2);

  transport.fail_next_send = true;
  LspClientStop(&client);
  CHECK_EQ(response.call_count, (u32)1);
  CHECK(response.has_error);
  CHECK_EQ(response.error_code, (i64)kLspClientErrorStopped);

  LspClientDestroy(&client);
}

TEST(lsp_client_stop_preserves_stderr_summary) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "stop_stderr");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{}}"));
  transport.stderr_summary = "last stderr line";

  LspClientStop(&client);
  CHECK_STR(LspClientGetStderrSummary(&client), Str8Lit("last stderr line"));

  LspClientDestroy(&client);
}

TEST(lsp_client_real_transport_stop_delivers_exit_before_force) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "real_stop");
  WakeCounter wake = {};
  LspClient client = {};

  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("real_stop.jsonl"));
  JsonWriter script_writer = {};
  JsonWriterInit(&script_writer, scope.arena);
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("recordPath")));
  CHECK(JsonWriteString(&script_writer, record_path));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("onMessage")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("initialize")));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("initialized")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("shutdown")));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sendFrame")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}")));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("sleepMs")));
  CHECK(JsonWriteU64(&script_writer, 150));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("expectMethod")));
  CHECK(JsonWriteString(&script_writer, Str8Lit("exit")));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("actions")));
  CHECK(JsonWriteArrayBegin(&script_writer));
  CHECK(JsonWriteObjectBegin(&script_writer));
  CHECK(JsonWriteObjectKey(&script_writer, Str8Lit("exitCode")));
  CHECK(JsonWriteI64(&script_writer, 0));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  CHECK(JsonWriteArrayEnd(&script_writer));
  CHECK(JsonWriteObjectEnd(&script_writer));
  String8 script = FinishJsonWriterOrFail(&script_writer);
  JsonWriterDestroy(&script_writer);
  String8 script_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("real_stop_script.json"), script);

  String8 args[] = {Str8Lit("--script"), script_path};
  LspClientConfig config = {};
  config.command = MakeFakeServerCommand(scope.arena, fixture.path, {args[0], args[1]});
  config.workspace_root = PushStr8Copy(scope.arena, fixture.path);
  config.wake = WakeCounterProc;
  config.wake_user_data = &wake;
  config.shutdown_timeout_ms = 400;
  CHECK(LspClientConfigure(&client, &config));
  CHECK(LspClientStart(&client));

  CHECK(WaitUntil([&]() {
    LspClientTick(&client);
    return LspClientGetState(&client) == LspClientState::Ready;
  }, 1500));

  LspClientStop(&client);
  CHECK(WaitUntil([&]() { return FileHasContents(record_path); }, 400));
  String8 lines = ReadFileOrFail(scope.arena, record_path);
  CHECK(Str8FindFirst(lines, Str8Lit("\"method\":\"exit\"")) < lines.size);

  LspClientDestroy(&client);
}

TEST(lsp_client_fake_server_script_exit_zero_terminates) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "exit_zero");

  JsonWriter writer = {};
  JsonWriterInit(&writer, scope.arena);
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("startup")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("exitCode")));
  CHECK(JsonWriteI64(&writer, 0));
  CHECK(JsonWriteObjectEnd(&writer));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));
  String8 script = FinishJsonWriterOrFail(&writer);
  JsonWriterDestroy(&writer);
  String8 script_path = WriteTextFile(scope.arena, fixture.path, Str8Lit("exit_zero_script.json"), script);

  String8 arguments[] = {Str8Lit("--script"), script_path};
  OsProcessCommand command = {Str8C(EDITOR_FAKE_LSP_PATH), arguments, ArrayCount(arguments), fixture.path};
  OsProcess process = {};
  CHECK(OsProcessStart(&process, &command));

  i32 exit_code = -1;
  CHECK(WaitUntil([&]() { return OsProcessTryWait(&process, &exit_code); }, 250));
  CHECK_EQ(exit_code, 0);

  OsProcessDestroy(&process);
}

TEST(lsp_client_destroying_callback_completes_later_pending_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "destroy_pending_tail");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};
  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{}}"));

  DestroyClientState destroy_state = {};
  destroy_state.client = &client;
  ResponseRecord tail_response = {};
  u64 first_id =
      LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), nullptr,
                               DestroyClientOnResponse, &destroy_state);
  u64 second_id = LspClientSendRequestJson(&client, Str8Lit("textDocument/definition"), Str8Lit("{}"),
                                           nullptr, CaptureResponse, &tail_response);
  CHECK(first_id != 0);
  CHECK(second_id == first_id + 1);

  transport.PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"value\":\"ok\"}}",
                               (unsigned long long)first_id));
  LspClientTick(&client);

  CHECK_EQ(destroy_state.call_count, (u32)1);
  CHECK_EQ(tail_response.call_count, (u32)1);
  CHECK(tail_response.has_error);
  CHECK_EQ(tail_response.error_code, (i64)kLspClientErrorStopped);
  CHECK(client.impl == nullptr);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Stopped);
  LspClientTick(&client);
}

TEST(lsp_client_handles_server_requests_and_unknown_methods) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "server_requests");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"workspace\":{\"workspaceEdit\":{\"documentChanges\":true}}}}"));

  callbacks.apply_edit.next_applied = true;
  callbacks.apply_edit.failure_reason.clear();
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":\"workspace/configuration\","
                             "\"params\":{\"items\":[{},{}]}}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":901,\"method\":\"workspace/workspaceFolders\"}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":902,\"method\":\"client/registerCapability\","
                             "\"params\":{\"registrations\":[]}}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":903,\"method\":\"client/unregisterCapability\","
                             "\"params\":{\"unregisterations\":[]}}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":904,\"method\":\"workspace/applyEdit\","
                             "\"params\":{\"label\":\"good\",\"edit\":{\"changes\":{}}}}"));
  LspClientTick(&client);

  CHECK_EQ(callbacks.apply_edit.call_count, (u32)1);
  CHECK_EQ(transport.sent.size(), (size_t)7);

  JsonValue *configuration = ParseJsonOrFail(scope.arena, Str8C(transport.sent[2].c_str()));
  CHECK_EQ(JsonArrayCount(JsonObjectGet(configuration, Str8Lit("result"))), (u64)2);
  CHECK(JsonArrayItem(JsonObjectGet(configuration, Str8Lit("result")), 0)->kind == JsonKind::Null);
  CHECK(JsonArrayItem(JsonObjectGet(configuration, Str8Lit("result")), 1)->kind == JsonKind::Null);

  JsonValue *workspace_folders = ParseJsonOrFail(scope.arena, Str8C(transport.sent[3].c_str()));
  JsonValue *folders_result = JsonObjectGet(workspace_folders, Str8Lit("result"));
  CHECK_EQ(JsonArrayCount(folders_result), (u64)1);
  String8 folder_uri = {};
  CHECK(JsonGetString(JsonObjectGet(JsonArrayItem(folders_result, 0), Str8Lit("uri")), &folder_uri));
  CHECK_STR(folder_uri, LspPathToUri(scope.arena, fixture.path));

  JsonValue *register_ok = ParseJsonOrFail(scope.arena, Str8C(transport.sent[4].c_str()));
  CHECK(JsonObjectGet(register_ok, Str8Lit("result"))->kind == JsonKind::Null);
  JsonValue *unregister_ok = ParseJsonOrFail(scope.arena, Str8C(transport.sent[5].c_str()));
  CHECK(JsonObjectGet(unregister_ok, Str8Lit("result"))->kind == JsonKind::Null);

  JsonValue *apply_ok = ParseJsonOrFail(scope.arena, Str8C(transport.sent[6].c_str()));
  CHECK(JsonObjectGet(JsonObjectGet(apply_ok, Str8Lit("result")), Str8Lit("applied"))->boolean);

  callbacks.apply_edit.next_applied = false;
  callbacks.apply_edit.failure_reason = "apply edit rejected";
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":905,\"method\":\"workspace/applyEdit\","
                             "\"params\":{\"label\":\"bad\",\"edit\":{\"changes\":{}}}}"));
  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":906,\"method\":\"workspace/unknown\"}"));
  LspClientTick(&client);

  CHECK_EQ(callbacks.apply_edit.call_count, (u32)2);
  CHECK_EQ(transport.sent.size(), (size_t)9);

  JsonValue *apply_fail = ParseJsonOrFail(scope.arena, Str8C(transport.sent[7].c_str()));
  CHECK(!JsonObjectGet(JsonObjectGet(apply_fail, Str8Lit("result")), Str8Lit("applied"))->boolean);
  String8 failure_reason = {};
  CHECK(JsonGetString(JsonObjectGet(JsonObjectGet(apply_fail, Str8Lit("result")), Str8Lit("failureReason")),
                      &failure_reason));
  CHECK_STR(failure_reason, Str8Lit("apply edit rejected"));

  JsonValue *unknown = ParseJsonOrFail(scope.arena, Str8C(transport.sent[8].c_str()));
  JsonValue *error = JsonObjectGet(unknown, Str8Lit("error"));
  i64 error_code = 0;
  String8 error_message = {};
  CHECK(JsonGetI64(JsonObjectGet(error, Str8Lit("code")), &error_code));
  CHECK(JsonGetString(JsonObjectGet(error, Str8Lit("message")), &error_message));
  CHECK_EQ(error_code, (i64)-32601);
  CHECK_STR(error_message, Str8Lit("Method not found"));

  LspClientDestroy(&client);
}

TEST(lsp_client_apply_edit_callback_may_destroy_client_after_reply) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "apply_edit_destroy");
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};
  DestroyClientState apply_edit = {};
  apply_edit.client = &client;

  LspClientCallbacks callbacks = {};
  callbacks.apply_edit = DestroyDuringApplyEdit;
  callbacks.user_data = &apply_edit;

  LspClientConfig config = {};
  config.command = MakeFakeServerCommand(scope.arena, fixture.path, {});
  config.workspace_root = PushStr8Copy(scope.arena, fixture.path);
  config.wake = WakeCounterProc;
  config.wake_user_data = &wake;
  config.callbacks = callbacks;
  config.transport_hooks = MockTransport::Hooks();
  config.transport_user_data = &transport;
  config.shutdown_timeout_ms = 40;

  CHECK(LspClientConfigure(&client, &config));
  u64 initialize_id = 0;
  CHECK(LspClientStart(&client));
  {
    JsonValue *initialize = ParseJsonOrFail(scope.arena, Str8C(transport.sent[0].c_str()));
    initialize_id = JsonIdOrFail(initialize);
  }
  transport.PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"capabilities\":{}}}",
                               (unsigned long long)initialize_id));
  LspClientTick(&client);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Ready);

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":\"workspace/applyEdit\","
                             "\"params\":{\"edit\":{\"changes\":{}}}}"));
  LspClientTick(&client);

  CHECK_EQ(apply_edit.call_count, (u32)1);
  CHECK(client.impl == nullptr);
  CHECK(transport.sent.size() >= (size_t)3);
  JsonValue *apply_reply = ParseJsonOrFail(scope.arena, Str8C(transport.sent[2].c_str()));
  CHECK_EQ(JsonIdOrFail(apply_reply), (u64)900);
  CHECK(JsonObjectGet(JsonObjectGet(apply_reply, Str8Lit("result")), Str8Lit("applied"))->boolean);
}

TEST(lsp_client_dispatches_notifications) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "notifications");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                             "\"params\":{\"uri\":\"file:///tmp/a.cpp\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},\"message\":\"x\"}]}}"));
  transport.PushJson(Str8Lit(
      "{\"jsonrpc\":\"2.0\",\"method\":\"window/showMessage\",\"params\":{\"type\":3,\"message\":\"hello\"}}"));
  transport.PushJson(Str8Lit(
      "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"type\":4,\"message\":\"trace\"}}"));
  LspClientTick(&client);

  CHECK_EQ(callbacks.notifications.size(), (size_t)3);
  CHECK_EQ(callbacks.notifications[0].count, (u64)1);
  CHECK_STR(Str8C(callbacks.notifications[0].method.c_str()), Str8Lit("textDocument/publishDiagnostics"));
  CHECK_STR(Str8C(callbacks.notifications[0].uri.c_str()), Str8Lit("file:///tmp/a.cpp"));
  CHECK_STR(Str8C(callbacks.notifications[1].method.c_str()), Str8Lit("window/showMessage"));
  CHECK_STR(Str8C(callbacks.notifications[1].message.c_str()), Str8Lit("hello"));
  CHECK_STR(Str8C(callbacks.notifications[2].method.c_str()), Str8Lit("window/logMessage"));
  CHECK_STR(Str8C(callbacks.notifications[2].message.c_str()), Str8Lit("trace"));

  LspClientDestroy(&client);
}

TEST(lsp_client_malformed_messages_fail_and_drain_pending_once) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "malformed");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  ResponseRecord first = {};
  ResponseRecord second = {};
  LspClientRequestContext context_one = {1, 2, 3, 4, &first};
  LspClientRequestContext context_two = {5, 6, 7, 8, &second};
  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/hover"), Str8Lit("{}"), &context_one,
                                    CaptureResponse, &first),
           (u64)2);
  CHECK_EQ(LspClientSendRequestJson(&client, Str8Lit("textDocument/definition"), Str8Lit("{}"), &context_two,
                                    CaptureResponse, &second),
           (u64)3);

  transport.PushJson(Str8Lit("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":"));
  LspClientTick(&client);

  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Failed);
  CHECK(LspClientGetFailureReason(&client).size > 0);
  CHECK_EQ(first.call_count, (u32)1);
  CHECK_EQ(second.call_count, (u32)1);
  CHECK(first.has_error);
  CHECK(second.has_error);

  LspClientDestroy(&client);
}

TEST(lsp_client_shutdown_sends_shutdown_then_exit_and_forced_stop_is_bounded) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "shutdown");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  transport.auto_shutdown_response = true;
  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  LspClientStop(&client);
  CHECK(transport.stopped);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Stopped);
  CHECK(transport.sent.size() >= 4);
  JsonValue *shutdown = ParseJsonOrFail(scope.arena, Str8C(transport.sent[2].c_str()));
  ExpectMethod(shutdown, Str8Lit("shutdown"));
  JsonValue *exit = ParseJsonOrFail(scope.arena, Str8C(transport.sent.back().c_str()));
  ExpectMethod(exit, Str8Lit("exit"));
  LspClientStop(&client);
  LspClientDestroy(&client);

  CallbackState forced_callbacks = {};
  MockTransport forced_transport = {};
  WakeCounter forced_wake = {};
  LspClient forced_client = {};
  (void)StartReadyClient(scope.arena, &forced_client, &forced_transport, &forced_callbacks, &forced_wake,
                         fixture.path, Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  Clock::time_point start = Clock::now();
  LspClientStop(&forced_client);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  CHECK(elapsed.count() < 750);
  CHECK(forced_transport.stopped);
  CHECK_EQ((u64)LspClientGetState(&forced_client), (u64)LspClientState::Stopped);
  JsonValue *forced_shutdown = ParseJsonOrFail(scope.arena, Str8C(forced_transport.sent[2].c_str()));
  ExpectMethod(forced_shutdown, Str8Lit("shutdown"));
  CHECK_EQ(forced_transport.sent.size(), (size_t)3);

  LspClientDestroy(&forced_client);
}

TEST(lsp_client_restart_budget_allows_only_one_restart) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "restart");
  CallbackState callbacks = {};
  MockTransport transport = {};
  WakeCounter wake = {};
  LspClient client = {};

  (void)StartReadyClient(scope.arena, &client, &transport, &callbacks, &wake, fixture.path,
                         Str8Lit("{\"capabilities\":{\"hoverProvider\":true}}"));

  transport.MarkFailed(Str8Lit("server crashed"), Str8Lit("stderr one"));
  LspClientTick(&client);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Failed);
  CHECK(transport.stopped);
  CHECK(LspClientCanRestart(&client));
  CHECK_STR(LspClientGetStderrSummary(&client), Str8Lit("stderr one"));
  CHECK(!LspClientStart(&client));

  transport.ResetForRestart();
  CHECK(LspClientRestart(&client));
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Initializing);
  JsonValue *restart_initialize = ParseJsonOrFail(scope.arena, Str8C(transport.sent[0].c_str()));
  u64 restart_id = JsonIdOrFail(restart_initialize);
  transport.PushJson(PushStr8F(scope.arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"capabilities\":{}}}",
                               (unsigned long long)restart_id));
  LspClientTick(&client);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Ready);

  transport.MarkFailed(Str8Lit("server crashed again"), Str8Lit("stderr two"));
  LspClientTick(&client);
  CHECK_EQ((u64)LspClientGetState(&client), (u64)LspClientState::Failed);
  CHECK(transport.stopped);
  CHECK(!LspClientCanRestart(&client));
  CHECK(!LspClientRestart(&client));
  CHECK_STR(LspClientGetStderrSummary(&client), Str8Lit("stderr two"));

  LspClientDestroy(&client);
}

}  // namespace
