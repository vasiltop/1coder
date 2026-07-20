#include "editor/editor.h"
#include "editor/lsp.h"
#include "editor/lsp_ui.h"
#include "lsp/json.h"
#include "os/os_file.h"
#include "test.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char *kFakeLspModeEnv = "ONECODER_FAKE_LSP_MODE";
constexpr const char *kFakeLspRecordEnv = "ONECODER_FAKE_LSP_RECORD_PATH";

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
    path = PushStr8F(arena, "%.*s/build/test_lsp_e2e_%s_%d", (int)cwd.size, (char *)cwd.str, tag,
                     (int)serial);
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

struct ScenarioLayout {
  String8 root;
  String8 bin;
  String8 record_path;
};

struct ScopedEnvVar {
  Arena *arena;
  const char *name;
  bool had_old_value;
  String8 old_value;

  ScopedEnvVar(Arena *arena_, const char *name_, String8 value) : arena(arena_), name(name_) {
    const char *old = getenv(name);
    had_old_value = old != nullptr;
    if (had_old_value) old_value = PushStr8Copy(arena, Str8C(old));
    Set(value);
  }

  ~ScopedEnvVar() {
    if (had_old_value) {
      Set(old_value);
    } else {
#if defined(_WIN32)
      CHECK(_putenv_s(name, "") == 0);
#else
      CHECK(unsetenv(name) == 0);
#endif
    }
  }

  void Set(String8 value) {
#if defined(_WIN32)
    CHECK(_putenv_s(name, PushCStr(arena, value)) == 0);
#else
    CHECK(setenv(name, PushCStr(arena, value), 1) == 0);
#endif
  }
};

struct ScopedWorkingDirectory {
  Arena *arena;
  String8 old_value;

  explicit ScopedWorkingDirectory(Arena *arena_) : arena(arena_), old_value(OsGetCwd(arena_)) {}
  ~ScopedWorkingDirectory() { CHECK(Set(old_value)); }

  bool Set(String8 path) {
#if defined(_WIN32)
    return SetCurrentDirectoryA(PushCStr(arena, path)) != 0;
#else
    return chdir(PushCStr(arena, path)) == 0;
#endif
  }
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

String8 ExecutableFileName(Arena *arena, String8 bare_name) {
#if defined(_WIN32)
  return PushStr8Cat(arena, bare_name, Str8Lit(".exe"));
#else
  return PushStr8Copy(arena, bare_name);
#endif
}

bool CopyExecutableFixture(Arena *arena, String8 source_path, String8 dest_path) {
  TempArena scratch = ScratchBegin();
  FileContents contents = OsFileRead(scratch.arena, source_path);
  bool ok = contents.ok && OsFileWrite(dest_path, contents.data);
  ScratchEnd(scratch);
  if (!ok) return false;
#if !defined(_WIN32)
  return chmod(PushCStr(arena, dest_path), 0777) == 0;
#else
  return true;
#endif
}

String8 InstallClangdAlias(Arena *arena, String8 bin_dir) {
  String8 alias_name = ExecutableFileName(arena, Str8Lit("clangd"));
  String8 alias_path = OsPathJoin(arena, bin_dir, alias_name);
  String8 source_path = Str8C(EDITOR_FAKE_LSP_PATH);
#if defined(_WIN32)
  CHECK(CopyFileA(PushCStr(arena, source_path), PushCStr(arena, alias_path), FALSE) != 0);
#else
  if (symlink(PushCStr(arena, source_path), PushCStr(arena, alias_path)) != 0) {
    CHECK(CopyExecutableFixture(arena, source_path, alias_path));
  }
#endif
  return alias_path;
}

String8 PrependPath(Arena *arena, String8 prefix) {
  const char *existing = getenv("PATH");
  if (existing == nullptr || existing[0] == 0) return PushStr8Copy(arena, prefix);
#if defined(_WIN32)
  return PushStr8F(arena, "%.*s;%s", (int)prefix.size, (char *)prefix.str, existing);
#else
  return PushStr8F(arena, "%.*s:%s", (int)prefix.size, (char *)prefix.str, existing);
#endif
}

Buffer *OpenAndShowFileBuffer(Editor *ed, String8 path) {
  BufferHandle handle = EditorOpenFile(ed, path);
  CHECK(handle.index != 0);
  EditorShowBuffer(ed, handle);
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  CHECK(buffer != nullptr);
  return buffer;
}

void FocusBuffer(Editor *ed, Buffer *buffer, u64 cursor) {
  View *view = EditorFocusedView(ed);
  CHECK(view != nullptr);
  view->buffer = buffer->handle;
  ViewSetCursor(view, buffer, cursor);
}

bool BufferDidOpen(Editor *ed, Buffer *buffer) {
  EditorLspBufferInfo info = {};
  return EditorLspGetBufferInfo(ed, buffer, &info) && info.did_open_sent;
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

u64 RecordedMethodCount(Arena *arena, String8 path, String8 method) {
  TempArena scratch = ScratchBegin1(arena);
  FileContents contents = OsFileRead(scratch.arena, path);
  if (!contents.ok || contents.data.size == 0) {
    ScratchEnd(scratch);
    return 0;
  }

  String8List lines = Str8SplitChar(scratch.arena, contents.data, '\n');
  u64 count = 0;
  for (String8Node *node = lines.first; node != nullptr; node = node->next) {
    if (node->string.size == 0) continue;
    JsonValue *root = ParseJsonOrFail(scratch.arena, node->string);
    String8 actual = {};
    if (!JsonGetString(JsonObjectGet(root, Str8Lit("method")), &actual)) continue;
    if (Str8Match(actual, method)) count += 1;
  }
  ScratchEnd(scratch);
  return count;
}

const char *PopupKindName(EditorLspUiPopupKind kind) {
  switch (kind) {
    case EditorLspUiPopupKind::None: return "none";
    case EditorLspUiPopupKind::Completion: return "completion";
    case EditorLspUiPopupKind::Hover: return "hover";
    case EditorLspUiPopupKind::Diagnostic: return "diagnostic";
  }
  return "unknown";
}

const char *ClientStateName(LspClientState state) {
  switch (state) {
    case LspClientState::Stopped: return "stopped";
    case LspClientState::Starting: return "starting";
    case LspClientState::Initializing: return "initializing";
    case LspClientState::Ready: return "ready";
    case LspClientState::Failed: return "failed";
    case LspClientState::Stopping: return "stopping";
  }
  return "unknown";
}

std::string ToStdString(String8 text) {
  if (text.str == nullptr || text.size == 0) return {};
  return std::string((const char *)text.str, (size_t)text.size);
}

std::string DescribeEditorState(Editor *ed, String8 record_path) {
  std::string focused = "<none>";
  if (Buffer *buffer = EditorFocusedBuffer(ed)) {
    focused = buffer->path.size > 0 ? ToStdString(buffer->path) : ToStdString(buffer->name);
  }

  std::string state = "status='" + ToStdString(ed->status_message) + "'";
  state += " focused='" + focused + "'";
  state += " command_line=" + std::string(ed->command_line_active ? "true" : "false");
  state += " popup=" + std::string(PopupKindName(EditorLspUiPopup(ed->lsp_ui)->kind));
  state += " recorded=" + std::to_string((unsigned long long)RecordedMessageCount(record_path));

  Buffer *buffer = EditorFocusedBuffer(ed);
  EditorLspBufferInfo info = {};
  if (buffer != nullptr && EditorLspGetBufferInfo(ed, buffer, &info) && info.client != nullptr) {
    state += " client=" + std::string(ClientStateName(LspClientGetState(info.client)));
  }
  return state;
}

ScenarioLayout PrepareScenarioLayout(Arena *arena, String8 fixture_path, const char *record_name) {
  ScenarioLayout layout = {};
  layout.root = OsPathJoin(arena, fixture_path, Str8Lit("proj"));
  layout.bin = OsPathJoin(arena, fixture_path, Str8Lit("bin"));
  CHECK(OsMakeDirs(layout.root));
  CHECK(OsMakeDirs(layout.bin));
  CHECK(OsFileWrite(OsPathJoin(arena, layout.root, Str8Lit(".git")), String8{}));
  (void)InstallClangdAlias(arena, layout.bin);
  layout.record_path = OsPathJoin(arena, fixture_path, Str8C(record_name));
  return layout;
}

template <typename Predicate, typename DebugState>
void WaitUntilOrFail(const char *label, Predicate predicate, DebugState debug_state,
                     i32 timeout_ms = 2000) {
  Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (predicate()) return;
  std::string state = debug_state();
  TestFail(__FILE__, __LINE__, "%s timed out after %dms: %s", label, timeout_ms, state.c_str());
}

template <typename Predicate>
void WaitForEditor(Editor *ed, String8 record_path, const char *label, Predicate predicate,
                   i32 timeout_ms = 2000) {
  WaitUntilOrFail(label, predicate, [&]() { return DescribeEditorState(ed, record_path); },
                  timeout_ms);
}

EditorLspConfig RealRegistryConfig() {
  EditorLspConfig config = {};
  config.shutdown_timeout_ms = 60;
  return config;
}

TEST(lsp_e2e_real_process_lifecycle_completion_hover_and_shutdown) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "lifecycle");
  EditorScope editor(scope.arena);
  ScenarioLayout layout = PrepareScenarioLayout(scope.arena, fixture.path, "lifecycle.jsonl");
  String8 file_path = WriteTextFile(scope.arena, layout.root, Str8Lit("main.cpp"), Str8Lit("std::\n"));

  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(layout.root));
  ScopedEnvVar path(scope.arena, "PATH", PrependPath(scope.arena, layout.bin));
  ScopedEnvVar mode(scope.arena, kFakeLspModeEnv, Str8Lit("lifecycle"));
  ScopedEnvVar record(scope.arena, kFakeLspRecordEnv, layout.record_path);
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  EditorLspConfig config = RealRegistryConfig();
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer = OpenAndShowFileBuffer(&editor.ed, file_path);
  FocusBuffer(&editor.ed, buffer, BufferSize(buffer) - 1);

  WaitForEditor(&editor.ed, layout.record_path, "lifecycle diagnostics after didOpen", [&]() {
    (void)EditorTick(&editor.ed);
    u64 diagnostic_count = 0;
    const LspDiagnostic *diagnostics =
        EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &diagnostic_count);
    return BufferDidOpen(&editor.ed, buffer) && diagnostics != nullptr && diagnostic_count == 1;
  });

  u64 diagnostic_count = 0;
  const LspDiagnostic *diagnostics =
      EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &diagnostic_count);
  CHECK_EQ(diagnostic_count, (u64)1);
  CHECK(diagnostics != nullptr);
  CHECK_STR(diagnostics[0].message, Str8Lit("missing semicolon"));
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/didOpen")),
           (u64)1);

  EditorProcessSpec(&editor.ed, "A<C-Space>");
  WaitForEditor(&editor.ed, layout.record_path, "completion popup", [&]() {
    (void)EditorTick(&editor.ed);
    return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::Completion;
  });

  const EditorLspUiPopupView *popup = EditorLspUiPopup(editor.ed.lsp_ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Completion);
  CHECK_EQ(popup->completion.count, (u64)1);
  CHECK_STR(popup->completion.items[0].label, Str8Lit("vector"));
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/completion")),
           (u64)1);

  EditorProcessSpec(&editor.ed, "<CR>");
  WaitForEditor(&editor.ed, layout.record_path, "completion apply", [&]() {
    (void)EditorTick(&editor.ed);
    return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::None &&
           Str8Match(BufferTextAll(scope.arena, buffer), Str8Lit("std::vector"));
  });
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("std::vector"));
  CHECK(buffer->final_newline);

  EditorProcessSpec(&editor.ed, "<Esc><C-Space>");
  WaitForEditor(&editor.ed, layout.record_path, "hover popup", [&]() {
    (void)EditorTick(&editor.ed);
    return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::Hover;
  });

  popup = EditorLspUiPopup(editor.ed.lsp_ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Hover);
  CHECK_EQ(popup->text.line_count, (u64)1);
  CHECK_STR(popup->text.lines[0], Str8Lit("std::vector docs"));
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/hover")),
           (u64)1);

  EditorLspDisable(&editor.ed);
  WaitForEditor(&editor.ed, layout.record_path, "shutdown and exit", [&]() {
    return RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("shutdown")) == 1 &&
           RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("exit")) == 1;
  });
}

TEST(lsp_e2e_real_process_navigation_formatting_and_rename_workspace_edit) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "actions");
  EditorScope editor(scope.arena);
  ScenarioLayout layout = PrepareScenarioLayout(scope.arena, fixture.path, "actions.jsonl");
  String8 main_path =
      WriteTextFile(scope.arena, layout.root, Str8Lit("main.cpp"), Str8Lit("helper();\n"));
  String8 target_path =
      WriteTextFile(scope.arena, layout.root, Str8Lit("target.cpp"),
                    Str8Lit("void helper(){}\nint spacing=0;\n"));

  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(layout.root));
  ScopedEnvVar path(scope.arena, "PATH", PrependPath(scope.arena, layout.bin));
  ScopedEnvVar mode(scope.arena, kFakeLspModeEnv, Str8Lit("actions"));
  ScopedEnvVar record(scope.arena, kFakeLspRecordEnv, layout.record_path);
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  EditorLspConfig config = RealRegistryConfig();
  EditorLspEnable(&editor.ed, &config);

  Buffer *main_buffer = OpenAndShowFileBuffer(&editor.ed, main_path);
  FocusBuffer(&editor.ed, main_buffer, 0);

  WaitForEditor(&editor.ed, layout.record_path, "main didOpen", [&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, main_buffer);
  });

  EditorProcessSpec(&editor.ed, "gd");
  WaitForEditor(&editor.ed, layout.record_path, "definition navigation", [&]() {
    (void)EditorTick(&editor.ed);
    Buffer *focused = EditorFocusedBuffer(&editor.ed);
    View *view = EditorFocusedView(&editor.ed);
    return focused != nullptr && view != nullptr && Str8Match(focused->path, target_path) &&
           BufferDidOpen(&editor.ed, focused) && ViewCursorLine(view, focused) == 0 &&
           ViewCursorColumn(view, focused) == 5;
  });
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/definition")),
           (u64)1);

  Buffer *target_buffer = EditorFocusedBuffer(&editor.ed);
  CHECK(target_buffer != nullptr);
  CHECK_STR(target_buffer->path, target_path);

  EditorProcessSpec(&editor.ed, "<leader>cf");
  WaitForEditor(&editor.ed, layout.record_path, "formatting result", [&]() {
    (void)EditorTick(&editor.ed);
    return Str8Match(BufferTextAll(scope.arena, target_buffer),
                     Str8Lit("void helper(){}\nint spacing = 0;"));
  });
  CHECK(target_buffer->final_newline);
  CHECK(BufferIsDirty(target_buffer));
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/formatting")),
           (u64)1);

  EditorProcessSpec(&editor.ed, "<leader>rn");
  WaitForEditor(&editor.ed, layout.record_path, "rename prompt", [&]() {
    (void)EditorTick(&editor.ed);
    if (!editor.ed.command_line_active) return false;
    Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
    return command != nullptr && Str8Match(BufferTextAll(scope.arena, command), Str8Lit("helper"));
  });
  Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
  CHECK(command != nullptr);
  CHECK_STR(BufferTextAll(scope.arena, command), Str8Lit("helper"));
  CHECK_EQ(
      RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/prepareRename")),
      (u64)1);

  EditorProcessSpec(&editor.ed, "<C-w>renamed<CR>");
  WaitForEditor(&editor.ed, layout.record_path, "rename workspace edit", [&]() {
    (void)EditorTick(&editor.ed);
    return !editor.ed.command_line_active &&
           Str8Match(BufferTextAll(scope.arena, main_buffer), Str8Lit("renamed();")) &&
           Str8Match(BufferTextAll(scope.arena, target_buffer),
                     Str8Lit("void renamed(){}\nint spacing = 0;"));
  });

  CHECK(BufferIsDirty(main_buffer));
  CHECK(BufferIsDirty(target_buffer));
  CHECK(main_buffer->final_newline);
  CHECK(target_buffer->final_newline);
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/rename")),
           (u64)1);

  EditorLspDisable(&editor.ed);
  WaitForEditor(&editor.ed, layout.record_path, "actions shutdown and exit", [&]() {
    return RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("shutdown")) == 1 &&
           RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("exit")) == 1;
  });
}

TEST(lsp_e2e_real_process_restarts_once_after_crash) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "restart");
  EditorScope editor(scope.arena);
  ScenarioLayout layout = PrepareScenarioLayout(scope.arena, fixture.path, "restart.jsonl");
  String8 file_path = WriteTextFile(scope.arena, layout.root, Str8Lit("main.cpp"), Str8Lit("value\n"));

  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(layout.root));
  ScopedEnvVar path(scope.arena, "PATH", PrependPath(scope.arena, layout.bin));
  ScopedEnvVar mode(scope.arena, kFakeLspModeEnv, Str8Lit("crash"));
  ScopedEnvVar record(scope.arena, kFakeLspRecordEnv, layout.record_path);
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  EditorLspConfig config = RealRegistryConfig();
  EditorLspEnable(&editor.ed, &config);
  Buffer *buffer = OpenAndShowFileBuffer(&editor.ed, file_path);
  FocusBuffer(&editor.ed, buffer, 0);

  WaitForEditor(&editor.ed, layout.record_path, "initial didOpen", [&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer);
  });

  EditorProcessSpec(&editor.ed, "<C-Space>");
  WaitForEditor(&editor.ed, layout.record_path, "first server crash", [&]() {
    (void)EditorTick(&editor.ed);
    EditorLspBufferInfo info = {};
    return EditorLspGetBufferInfo(&editor.ed, buffer, &info) &&
           LspClientGetState(info.client) == LspClientState::Failed;
  });

  EditorProcessSpec(&editor.ed, "<C-Space>");
  EditorLspBufferInfo info = {};
  CHECK(EditorLspGetBufferInfo(&editor.ed, buffer, &info));
  CHECK_EQ((u64)LspClientGetState(info.client), (u64)LspClientState::Initializing);
  CHECK(!info.did_open_sent);
  CHECK(ToStdString(editor.ed.status_message).find("restarting") != std::string::npos);

  WaitForEditor(&editor.ed, layout.record_path, "restart didOpen", [&]() {
    (void)EditorTick(&editor.ed);
    return BufferDidOpen(&editor.ed, buffer) &&
           RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("initialize")) == 2 &&
           RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("textDocument/didOpen")) == 2;
  });

  EditorProcessSpec(&editor.ed, "<C-Space>");
  WaitForEditor(&editor.ed, layout.record_path, "second server crash", [&]() {
    (void)EditorTick(&editor.ed);
    EditorLspBufferInfo current = {};
    return EditorLspGetBufferInfo(&editor.ed, buffer, &current) &&
           LspClientGetState(current.client) == LspClientState::Failed &&
           !LspClientCanRestart(current.client);
  });

  EditorProcessSpec(&editor.ed, "<C-Space>");
  CHECK_EQ(RecordedMethodCount(scope.arena, layout.record_path, Str8Lit("initialize")), (u64)2);
  EditorProcessSpec(&editor.ed, "A!<Esc>");
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("value!"));
}

}  // namespace
