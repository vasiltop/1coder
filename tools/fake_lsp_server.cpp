#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "lsp/framing.h"
#include "lsp/json.h"
#include "os/os_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <errno.h>
#  include <unistd.h>
#endif

namespace {

enum class ActionKind : u8 {
  SendFrame,
  SendStdout,
  SendStderr,
  Sleep,
  GeneratedStderr,
  GeneratedFrameBurst,
  Exit,
  Crash,
};

struct Action {
  ActionKind kind;
  std::string text;
  std::vector<u64> fragments;
  u64 amount = 0;
  u64 count = 0;
  i64 exit_code = 0;
  std::string suffix;
  u8 fill = 'x';
};

struct Rule {
  std::string expect_method;
  std::vector<Action> actions;
};

struct Script {
  std::string record_path;
  std::vector<Action> startup;
  std::vector<Rule> on_message;
};

struct ActionRunResult {
  bool terminal = false;
  int exit_code = 0;
};

void SetBinaryMode() {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
}

std::string Copy(String8 s) { return std::string((const char *)s.str, (size_t)s.size); }

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t *value) {
  if (value == nullptr) return {};
  int need = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return {};
  std::string result;
  result.resize((size_t)need);
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), need, nullptr, nullptr);
  if (!result.empty()) result.pop_back();
  return result;
}

std::vector<std::string> Utf8Args(int fallback_argc, char **fallback_argv) {
  std::vector<std::string> result;
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr || argc <= 0) {
    if (argv) LocalFree(argv);
    result.reserve((size_t)fallback_argc);
    for (int i = 0; i < fallback_argc; i += 1) result.push_back(fallback_argv[i] ? fallback_argv[i] : "");
    return result;
  }

  result.reserve((size_t)argc);
  for (int i = 0; i < argc; i += 1) result.push_back(WideToUtf8(argv[i]));
  LocalFree(argv);
  return result;
}
#endif

bool GetStringField(const JsonValue *object, String8 key, std::string *out) {
  String8 value = {};
  if (!JsonGetString(JsonObjectGet(object, key), &value)) return false;
  *out = Copy(value);
  return true;
}

u64 GetU64Field(const JsonValue *object, String8 key, u64 fallback = 0) {
  u64 value = fallback;
  (void)JsonGetU64(JsonObjectGet(object, key), &value);
  return value;
}

std::vector<u64> DecodeFragments(const JsonValue *value) {
  std::vector<u64> result;
  if (value == nullptr || value->kind != JsonKind::Array) return result;
  result.reserve((size_t)value->array.count);
  for (u64 i = 0; i < value->array.count; i += 1) {
    u64 size = 0;
    if (JsonGetU64(value->array.items[i], &size)) result.push_back(size);
  }
  return result;
}

bool DecodeAction(const JsonValue *value, Action *out) {
  if (value == nullptr || value->kind != JsonKind::Object || out == nullptr) return false;

  std::string text = {};
  if (GetStringField(value, Str8Lit("sendFrame"), &text)) {
    out->kind = ActionKind::SendFrame;
    out->text = std::move(text);
    out->fragments = DecodeFragments(JsonObjectGet(value, Str8Lit("fragments")));
    return true;
  }
  if (GetStringField(value, Str8Lit("sendStdout"), &text)) {
    out->kind = ActionKind::SendStdout;
    out->text = std::move(text);
    out->fragments = DecodeFragments(JsonObjectGet(value, Str8Lit("fragments")));
    return true;
  }
  if (GetStringField(value, Str8Lit("sendStderr"), &text)) {
    out->kind = ActionKind::SendStderr;
    out->text = std::move(text);
    return true;
  }

  u64 sleep_ms = 0;
  if (JsonGetU64(JsonObjectGet(value, Str8Lit("sleepMs")), &sleep_ms)) {
    out->kind = ActionKind::Sleep;
    out->amount = sleep_ms;
    return true;
  }

  u64 generated_stderr = 0;
  if (JsonGetU64(JsonObjectGet(value, Str8Lit("generatedStderrBytes")), &generated_stderr)) {
    out->kind = ActionKind::GeneratedStderr;
    out->amount = generated_stderr;
    (void)GetStringField(value, Str8Lit("stderrPrefix"), &out->text);
    (void)GetStringField(value, Str8Lit("stderrSuffix"), &out->suffix);
    std::string fill = {};
    if (GetStringField(value, Str8Lit("stderrFill"), &fill) && !fill.empty()) out->fill = (u8)fill[0];
    return true;
  }

  u64 generated_frame = 0;
  if (JsonGetU64(JsonObjectGet(value, Str8Lit("generatedFrameBytes")), &generated_frame)) {
    out->kind = ActionKind::GeneratedFrameBurst;
    out->amount = generated_frame;
    out->count = GetU64Field(value, Str8Lit("generatedFrameCount"), 1);
    if (!GetStringField(value, Str8Lit("generatedFrameMethod"), &out->text)) return false;
    return true;
  }

  i64 exit_code = 0;
  if (JsonGetI64(JsonObjectGet(value, Str8Lit("exitCode")), &exit_code)) {
    out->kind = ActionKind::Exit;
    out->exit_code = exit_code;
    return true;
  }

  bool crash = false;
  if (JsonGetBool(JsonObjectGet(value, Str8Lit("crash")), &crash) && crash) {
    out->kind = ActionKind::Crash;
    return true;
  }

  return false;
}

bool DecodeRule(const JsonValue *value, Rule *out) {
  if (value == nullptr || value->kind != JsonKind::Object || out == nullptr) return false;
  (void)GetStringField(value, Str8Lit("expectMethod"), &out->expect_method);

  const JsonValue *actions = JsonObjectGet(value, Str8Lit("actions"));
  if (actions != nullptr) {
    if (actions->kind != JsonKind::Array) return false;
    out->actions.reserve((size_t)actions->array.count);
    for (u64 i = 0; i < actions->array.count; i += 1) {
      Action action = {};
      if (!DecodeAction(actions->array.items[i], &action)) return false;
      out->actions.push_back(std::move(action));
    }
  }

  bool crash = false;
  if (JsonGetBool(JsonObjectGet(value, Str8Lit("crash")), &crash) && crash) {
    Action action = {};
    action.kind = ActionKind::Crash;
    out->actions.push_back(std::move(action));
  }
  return true;
}

bool LoadScript(String8 path, Script *out) {
  Arena *arena = ArenaAlloc(MB(8));
  if (arena == nullptr) return false;

  FileContents contents = OsFileRead(arena, path);
  if (!contents.ok) {
    ArenaRelease(arena);
    return false;
  }

  JsonParseResult parsed = JsonParse(arena, contents.data);
  if (parsed.root == nullptr || parsed.root->kind != JsonKind::Object) {
    ArenaRelease(arena);
    return false;
  }

  out->record_path.clear();
  (void)GetStringField(parsed.root, Str8Lit("recordPath"), &out->record_path);

  const JsonValue *startup = JsonObjectGet(parsed.root, Str8Lit("startup"));
  if (startup != nullptr) {
    if (startup->kind != JsonKind::Array) {
      ArenaRelease(arena);
      return false;
    }
    out->startup.reserve((size_t)startup->array.count);
    for (u64 i = 0; i < startup->array.count; i += 1) {
      Action action = {};
      if (!DecodeAction(startup->array.items[i], &action)) {
        ArenaRelease(arena);
        return false;
      }
      out->startup.push_back(std::move(action));
    }
  }

  const JsonValue *on_message = JsonObjectGet(parsed.root, Str8Lit("onMessage"));
  if (on_message != nullptr) {
    if (on_message->kind != JsonKind::Array) {
      ArenaRelease(arena);
      return false;
    }
    out->on_message.reserve((size_t)on_message->array.count);
    for (u64 i = 0; i < on_message->array.count; i += 1) {
      Rule rule = {};
      if (!DecodeRule(on_message->array.items[i], &rule)) {
        ArenaRelease(arena);
        return false;
      }
      out->on_message.push_back(std::move(rule));
    }
  }

  ArenaRelease(arena);
  return true;
}

bool WriteBytes(FILE *stream, String8 bytes, const std::vector<u64> &fragments = {}) {
  size_t offset = 0;
  for (u64 fragment : fragments) {
    if (offset >= bytes.size) break;
    size_t size = (size_t)Min(fragment, bytes.size - offset);
    if (size == 0) continue;
    if (fwrite(bytes.str + offset, 1, size, stream) != size) return false;
    fflush(stream);
    offset += size;
  }
  if (offset < bytes.size) {
    size_t size = (size_t)(bytes.size - offset);
    if (fwrite(bytes.str + offset, 1, size, stream) != size) return false;
    fflush(stream);
  }
  return true;
}

i64 ReadStdin(u8 *buffer, size_t capacity) {
#if defined(_WIN32)
  return (i64)_read(_fileno(stdin), buffer, (unsigned int)capacity);
#else
  for (;;) {
    ssize_t got = read(STDIN_FILENO, buffer, capacity);
    if (got < 0 && errno == EINTR) continue;
    return (i64)got;
  }
#endif
}

std::string BuildRepeatedText(u64 total_bytes, const std::string &prefix, const std::string &suffix,
                              u8 fill) {
  std::string result;
  result.reserve((size_t)total_bytes);
  result.append(prefix);
  while (result.size() + suffix.size() < total_bytes) result.push_back((char)fill);
  if (result.size() < total_bytes) {
    size_t suffix_bytes = (size_t)Min((u64)suffix.size(), total_bytes - result.size());
    result.append(suffix.data(), suffix_bytes);
  }
  if (result.size() < total_bytes) result.resize((size_t)total_bytes, (char)fill);
  if (!suffix.empty() && total_bytes >= suffix.size()) {
    size_t at = result.size() - suffix.size();
    memcpy(result.data() + at, suffix.data(), suffix.size());
  }
  return result;
}

std::string BuildGeneratedFrameJson(String8 method, u64 blob_bytes) {
  std::string json = "{\"jsonrpc\":\"2.0\",\"method\":\"";
  json.append((const char *)method.str, (size_t)method.size);
  json.append("\",\"params\":{\"blob\":\"");
  json.append((size_t)blob_bytes, 'x');
  json.append("\"}}");
  return json;
}

void RecordMessage(FILE *record, String8 json) {
  if (record == nullptr) return;
  fwrite(json.str, 1, (size_t)json.size, record);
  fputc('\n', record);
  fflush(record);
}

FILE *OpenRecordFile(String8 path) {
  if (path.size == 0) return nullptr;

#if defined(_WIN32)
  int need = MultiByteToWideChar(CP_UTF8, 0, (const char *)path.str, (int)path.size, nullptr, 0);
  if (need <= 0) return nullptr;

  Arena *arena = ArenaAlloc(KB(64));
  if (arena == nullptr) return nullptr;

  wchar_t *wide = PushArrayNoZero(arena, wchar_t, (u64)need + 1);
  MultiByteToWideChar(CP_UTF8, 0, (const char *)path.str, (int)path.size, wide, need);
  wide[need] = 0;

  FILE *result = _wfopen(wide, L"ab");
  ArenaRelease(arena);
  return result;
#else
  Arena *arena = ArenaAlloc(KB(64));
  if (arena == nullptr) return nullptr;
  FILE *result = fopen(PushCStr(arena, path), "ab");
  ArenaRelease(arena);
  return result;
#endif
}

ActionRunResult RunActions(const std::vector<Action> &actions) {
  Arena *arena = ArenaAlloc(MB(2));
  if (arena == nullptr) return {true, 1};

  for (const Action &action : actions) {
    switch (action.kind) {
      case ActionKind::SendFrame: {
        String8 framed = LspFrameEncode(arena, Str8C(action.text.c_str()));
        if (!WriteBytes(stdout, framed, action.fragments)) {
          ArenaRelease(arena);
          return {true, 1};
        }
      } break;
      case ActionKind::SendStdout: {
        if (!WriteBytes(stdout, Str8C(action.text.c_str()), action.fragments)) {
          ArenaRelease(arena);
          return {true, 1};
        }
      } break;
      case ActionKind::SendStderr: {
        if (fwrite(action.text.data(), 1, action.text.size(), stderr) != action.text.size()) {
          ArenaRelease(arena);
          return {true, 1};
        }
        fflush(stderr);
      } break;
      case ActionKind::Sleep: {
        std::this_thread::sleep_for(std::chrono::milliseconds(action.amount));
      } break;
      case ActionKind::GeneratedStderr: {
        std::string text = BuildRepeatedText(action.amount, action.text, action.suffix, action.fill);
        if (fwrite(text.data(), 1, text.size(), stderr) != text.size()) {
          ArenaRelease(arena);
          return {true, 1};
        }
        fflush(stderr);
      } break;
      case ActionKind::GeneratedFrameBurst: {
        for (u64 i = 0; i < action.count; i += 1) {
          std::string json = BuildGeneratedFrameJson(Str8C(action.text.c_str()), action.amount);
          String8 framed = LspFrameEncode(arena, Str8C(json.c_str()));
          if (!WriteBytes(stdout, framed)) {
            ArenaRelease(arena);
            return {true, 1};
          }
          ArenaClear(arena);
        }
      } break;
      case ActionKind::Exit: {
        ArenaRelease(arena);
        return {true, (int)action.exit_code};
      } break;
      case ActionKind::Crash: {
        ArenaRelease(arena);
        return {true, 97};
      } break;
    }
    ArenaClear(arena);
  }

  ArenaRelease(arena);
  return {};
}

int EchoMode() {
  fputs("fake_lsp_server: stderr ready\n", stderr);
  fflush(stderr);

  unsigned char buffer[4096];
  for (;;) {
    size_t got = fread(buffer, 1, sizeof(buffer), stdin);
    if (got > 0) {
      if (fwrite(buffer, 1, got, stdout) != got) return 1;
      fflush(stdout);
    }
    if (got < sizeof(buffer)) {
      if (ferror(stdin)) return 1;
      if (feof(stdin)) return 0;
    }
  }
}

int SleepMode() {
#if defined(_WIN32)
  for (;;) Sleep(1000);
#else
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
}

int ScriptMode(String8 script_path) {
  Script script = {};
  if (!LoadScript(script_path, &script)) {
    fputs("fake_lsp_server: failed to load script\n", stderr);
    return 2;
  }

  FILE *record = nullptr;
  if (!script.record_path.empty()) {
    record = OpenRecordFile(Str8C(script.record_path.c_str()));
    if (record == nullptr) {
      fputs("fake_lsp_server: failed to open record file\n", stderr);
      return 2;
    }
  }

  ActionRunResult startup_result = RunActions(script.startup);
  if (startup_result.terminal) {
    if (record) fclose(record);
    return startup_result.exit_code;
  }

  Arena *arena = ArenaAlloc(MB(8));
  if (arena == nullptr) {
    if (record) fclose(record);
    return 2;
  }

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  u8 buffer[4096];
  size_t rule_index = 0;

  for (;;) {
    i64 got = ReadStdin(buffer, sizeof(buffer));
    if (got > 0 && !LspFrameDecoderFeed(&decoder, Str8(buffer, (u64)got))) {
      if (record) fclose(record);
      LspFrameDecoderDestroy(&decoder);
      ArenaRelease(arena);
      return 3;
    }

    while (LspFrameDecoderQueuedCount(&decoder) > 0) {
      ArenaClear(arena);
      String8 json = LspFrameDecoderPop(&decoder, arena);
      RecordMessage(record, json);

      if (rule_index < script.on_message.size()) {
        JsonParseResult parsed = JsonParse(arena, json);
        if (parsed.root == nullptr || parsed.root->kind != JsonKind::Object) {
          if (record) fclose(record);
          LspFrameDecoderDestroy(&decoder);
          ArenaRelease(arena);
          return 4;
        }

        String8 method = {};
        if (!JsonGetString(JsonObjectGet(parsed.root, Str8Lit("method")), &method)) {
          if (record) fclose(record);
          LspFrameDecoderDestroy(&decoder);
          ArenaRelease(arena);
          return 4;
        }

        Rule &rule = script.on_message[rule_index];
        if (!rule.expect_method.empty() &&
            !Str8Match(method, Str8C(rule.expect_method.c_str()))) {
          if (record) fclose(record);
          LspFrameDecoderDestroy(&decoder);
          ArenaRelease(arena);
          return 5;
        }

        ActionRunResult action_result = RunActions(rule.actions);
        if (action_result.terminal) {
          if (record) fclose(record);
          LspFrameDecoderDestroy(&decoder);
          ArenaRelease(arena);
          return action_result.exit_code;
        }
        rule_index += 1;
      }
    }

    if (got < 0) {
      if (record) fclose(record);
      LspFrameDecoderDestroy(&decoder);
      ArenaRelease(arena);
      return 1;
    }
    if (got == 0) break;
  }

  int result = LspFrameDecoderFinish(&decoder) ? 0 : 3;
  if (record) fclose(record);
  LspFrameDecoderDestroy(&decoder);
  ArenaRelease(arena);
  return result;
}

std::string EnvValue(const char *name) {
  const char *value = getenv(name);
  return value ? value : "";
}

std::string JsonTextDocumentUri(const JsonValue *root) {
  if (root == nullptr) return {};
  const JsonValue *params = JsonObjectGet(root, Str8Lit("params"));
  const JsonValue *document = JsonObjectGet(params, Str8Lit("textDocument"));
  std::string uri = {};
  (void)GetStringField(document, Str8Lit("uri"), &uri);
  return uri;
}

std::string UriSibling(const std::string &uri, const char *file_name) {
  size_t slash = uri.rfind('/');
  if (slash == std::string::npos) return file_name ? file_name : "";
  return uri.substr(0, slash + 1) + (file_name ? file_name : "");
}

bool SendJsonFrame(Arena *arena, String8 json) {
  String8 framed = LspFrameEncode(arena, json);
  return WriteBytes(stdout, framed);
}

int E2eReturn(FILE *record, LspFrameDecoder *decoder, Arena *arena, int code) {
  if (record) fclose(record);
  if (decoder) LspFrameDecoderDestroy(decoder);
  if (arena) ArenaRelease(arena);
  return code;
}

int E2eMode(String8 scenario) {
  std::string record_path = EnvValue("ONECODER_FAKE_LSP_RECORD_PATH");
  FILE *record = record_path.empty() ? nullptr : OpenRecordFile(Str8C(record_path.c_str()));
  if (!record_path.empty() && record == nullptr) {
    fputs("fake_lsp_server: failed to open e2e record file\n", stderr);
    return 2;
  }

  Arena *arena = ArenaAlloc(MB(8));
  if (arena == nullptr) return E2eReturn(record, nullptr, arena, 2);

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  u8 buffer[4096];

  for (;;) {
    i64 got = ReadStdin(buffer, sizeof(buffer));
    if (got > 0 && !LspFrameDecoderFeed(&decoder, Str8(buffer, (u64)got))) {
      return E2eReturn(record, &decoder, arena, 3);
    }

    while (LspFrameDecoderQueuedCount(&decoder) > 0) {
      ArenaClear(arena);
      String8 json = LspFrameDecoderPop(&decoder, arena);
      RecordMessage(record, json);

      JsonParseResult parsed = JsonParse(arena, json);
      if (parsed.root == nullptr || parsed.root->kind != JsonKind::Object) {
        return E2eReturn(record, &decoder, arena, 4);
      }

      String8 method = {};
      if (!JsonGetString(JsonObjectGet(parsed.root, Str8Lit("method")), &method)) {
        return E2eReturn(record, &decoder, arena, 4);
      }

      u64 id = 0;
      bool has_id = JsonGetU64(JsonObjectGet(parsed.root, Str8Lit("id")), &id);
      bool crash_mode = Str8Match(scenario, Str8Lit("crash"));
      if (crash_mode && !Str8Match(method, Str8Lit("initialize")) &&
          !Str8Match(method, Str8Lit("initialized")) &&
          !Str8Match(method, Str8Lit("textDocument/didOpen")) &&
          !Str8Match(method, Str8Lit("shutdown")) && !Str8Match(method, Str8Lit("exit"))) {
        return E2eReturn(record, &decoder, arena, 97);
      }

      String8 response = {};
      if (Str8Match(method, Str8Lit("initialize"))) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"capabilities\":{"
            "\"positionEncoding\":\"utf-8\","
            "\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
            "\"completionProvider\":{},"
            "\"hoverProvider\":true,"
            "\"definitionProvider\":true,"
            "\"documentFormattingProvider\":true,"
            "\"renameProvider\":{\"prepareProvider\":true}}}}",
            (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("textDocument/didOpen"))) {
        if (Str8Match(scenario, Str8Lit("lifecycle"))) {
          std::string uri = JsonTextDocumentUri(parsed.root);
          response = PushStr8F(
              arena,
              "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
              "\"params\":{\"uri\":\"%s\",\"version\":1,\"diagnostics\":["
              "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":5}},\"severity\":1,"
              "\"message\":\"missing semicolon\"}]}}",
              uri.c_str());
        }
      } else if (Str8Match(method, Str8Lit("textDocument/completion")) && has_id) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":[{\"label\":\"vector\","
            "\"insertText\":\"vector\"}]}",
            (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("textDocument/hover")) && has_id) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"contents\":\"std::vector docs\"}}",
            (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("textDocument/definition")) && has_id) {
        std::string source_uri = JsonTextDocumentUri(parsed.root);
        std::string target_uri = UriSibling(source_uri, "target.cpp");
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":[{\"uri\":\"%s\","
            "\"range\":{\"start\":{\"line\":0,\"character\":5},"
            "\"end\":{\"line\":0,\"character\":11}}}]}",
            (unsigned long long)id, target_uri.c_str());
      } else if (Str8Match(method, Str8Lit("textDocument/formatting")) && has_id) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":[{\"range\":{\"start\":{\"line\":1,"
            "\"character\":0},\"end\":{\"line\":1,\"character\":14}},"
            "\"newText\":\"int spacing = 0;\"}]}",
            (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("textDocument/prepareRename")) && has_id) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"range\":{\"start\":{\"line\":0,"
            "\"character\":5},\"end\":{\"line\":0,\"character\":11}},"
            "\"placeholder\":\"helper\"}}",
            (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("textDocument/rename")) && has_id) {
        std::string current_uri = JsonTextDocumentUri(parsed.root);
        std::string main_uri = UriSibling(current_uri, "main.cpp");
        std::string new_name = {};
        const JsonValue *params = JsonObjectGet(parsed.root, Str8Lit("params"));
        (void)GetStringField(params, Str8Lit("newName"), &new_name);
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":{\"changes\":{\"%s\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":5},\"end\":{\"line\":0,"
            "\"character\":11}},\"newText\":\"%s\"}],\"%s\":[{\"range\":{\"start\":"
            "{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":6}},"
            "\"newText\":\"%s\"}]}}}",
            (unsigned long long)id, current_uri.c_str(), new_name.c_str(), main_uri.c_str(),
            new_name.c_str());
      } else if (Str8Match(method, Str8Lit("shutdown")) && has_id) {
        response = PushStr8F(arena, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":null}",
                             (unsigned long long)id);
      } else if (Str8Match(method, Str8Lit("exit"))) {
        return E2eReturn(record, &decoder, arena, 0);
      } else if (!Str8Match(method, Str8Lit("initialized")) && has_id) {
        response = PushStr8F(
            arena,
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"error\":{\"code\":-32601,\"message\":"
            "\"method not supported\"}}",
            (unsigned long long)id);
      }

      if (response.size > 0 && !SendJsonFrame(arena, response)) {
        return E2eReturn(record, &decoder, arena, 1);
      }
    }

    if (got < 0) return E2eReturn(record, &decoder, arena, 1);
    if (got == 0) break;
  }

  int result = LspFrameDecoderFinish(&decoder) ? 0 : 3;
  return E2eReturn(record, &decoder, arena, result);
}

}  // namespace

int main(int argc, char **argv) {
  SetBinaryMode();

#if defined(_WIN32)
  std::vector<std::string> utf8_args = Utf8Args(argc, argv);
  if (!utf8_args.empty()) {
    argc = (int)utf8_args.size();
    if (argc > 1) argv[1] = utf8_args[1].empty() ? nullptr : utf8_args[1].data();
    if (argc > 2) argv[2] = utf8_args[2].empty() ? nullptr : utf8_args[2].data();
  }
#endif

  std::string e2e_mode = EnvValue("ONECODER_FAKE_LSP_MODE");
  if (!e2e_mode.empty()) return E2eMode(Str8C(e2e_mode.c_str()));

  if (argc > 1 && strcmp(argv[1], "--echo") == 0) return EchoMode();
  if (argc > 1 && strcmp(argv[1], "--sleep") == 0) return SleepMode();
  if (argc > 2 && strcmp(argv[1], "--script") == 0) return ScriptMode(Str8C(argv[2]));

  fputs("fake_lsp_server: expected --echo, --sleep, or --script <path>\n", stderr);
  return 2;
}
