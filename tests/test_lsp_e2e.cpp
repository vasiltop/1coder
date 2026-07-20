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
      CHECK(SetEnvironmentVariableA(name, nullptr) != 0);
#else
      CHECK(unsetenv(name) == 0);
#endif
    }
  }

  void Set(String8 value) {
#if defined(_WIN32)
    CHECK(SetEnvironmentVariableA(name, PushCStr(arena, value)) != 0);
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

View *FocusBuffer(Editor *ed, Buffer *buffer, u64 cursor) {
  View *view = EditorFocusedView(ed);
  CHECK(view != nullptr);
  view->buffer = buffer->handle;
  ViewSetCursor(view, buffer, cursor);
  return view;
}

bool BufferDidOpen(Editor *ed, Buffer *buffer) {
  EditorLspBufferInfo info = {};
  return EditorLspGetBufferInfo(ed, buffer, &info) && info.did_open_sent;
}

bool StatusContains(Editor *ed, const char *needle) {
  if (ed == nullptr || ed->status_message.str == nullptr || needle == nullptr) return false;
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

u64 CountRecordedMethod(Arena *arena, String8 path, String8 method) {
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

EditorLspConfig RealRegistryConfig() {
  EditorLspConfig config = {};
  config.shutdown_timeout_ms = 60;
  return config;
}

TEST(lsp_e2e_real_process_lifecycle_completion_hover_and_shutdown) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "lifecycle");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  String8 bin = OsPathJoin(scope.arena, fixture.path, Str8Lit("bin"));
  CHECK(OsMakeDirs(root));
  CHECK(OsMakeDirs(bin));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));
  (void)InstallClangdAlias(scope.arena, bin);

  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("lifecycle.jsonl"));
  String8 file_path = WriteTextFile(scope.arena, root, Str8Lit("main.cpp"), Str8Lit("std::\n"));

  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(root));
  ScopedEnvVar path(scope.arena, "PATH", PrependPath(scope.arena, bin));
  ScopedEnvVar mode(scope.arena, kFakeLspModeEnv, Str8Lit("lifecycle"));
  ScopedEnvVar record(scope.arena, kFakeLspRecordEnv, record_path);
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  EditorLspConfig config = RealRegistryConfig();
  EditorLspEnable(&editor.ed, &config);

  Buffer *buffer = OpenAndShowFileBuffer(&editor.ed, file_path);
  FocusBuffer(&editor.ed, buffer, BufferSize(buffer) - 1);

  WaitUntilOrFail(
      "lifecycle diagnostics after didOpen",
      [&]() {
        (void)EditorTick(&editor.ed);
        u64 diagnostic_count = 0;
        const LspDiagnostic *diagnostics =
            EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &diagnostic_count);
        return BufferDidOpen(&editor.ed, buffer) && diagnostics != nullptr && diagnostic_count == 1;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });

  u64 diagnostic_count = 0;
  const LspDiagnostic *diagnostics =
      EditorLspUiDiagnosticsForBuffer(editor.ed.lsp_ui, buffer, &diagnostic_count);
  CHECK_EQ(diagnostic_count, (u64)1);
  CHECK(diagnostics != nullptr);
  CHECK_STR(diagnostics[0].message, Str8Lit("missing semicolon"));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/didOpen")), (u64)1);

  EditorProcessSpec(&editor.ed, "A<C-Space>");
  WaitUntilOrFail(
      "completion popup",
      [&]() {
        (void)EditorTick(&editor.ed);
        return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::Completion;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });

  const EditorLspUiPopupView *popup = EditorLspUiPopup(editor.ed.lsp_ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Completion);
  CHECK_EQ(popup->completion.count, (u64)1);
  CHECK_STR(popup->completion.items[0].label, Str8Lit("vector"));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/completion")), (u64)1);

  EditorProcessSpec(&editor.ed, "<CR>");
  WaitUntilOrFail(
      "completion apply",
      [&]() {
        (void)EditorTick(&editor.ed);
        return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::None &&
               Str8Match(BufferTextAll(scope.arena, buffer), Str8Lit("std::vector\n"));
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
  CHECK_STR(BufferTextAll(scope.arena, buffer), Str8Lit("std::vector\n"));

  EditorProcessSpec(&editor.ed, "<Esc><C-Space>");
  WaitUntilOrFail(
      "hover popup",
      [&]() {
        (void)EditorTick(&editor.ed);
        return EditorLspUiPopup(editor.ed.lsp_ui)->kind == EditorLspUiPopupKind::Hover;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });

  popup = EditorLspUiPopup(editor.ed.lsp_ui);
  CHECK_EQ((u64)popup->kind, (u64)EditorLspUiPopupKind::Hover);
  CHECK_EQ(popup->text.line_count, (u64)1);
  CHECK_STR(popup->text.lines[0], Str8Lit("std::vector docs"));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/hover")), (u64)1);

  EditorLspDisable(&editor.ed);
  WaitUntilOrFail(
      "shutdown and exit",
      [&]() {
        return CountRecordedMethod(scope.arena, record_path, Str8Lit("shutdown")) == 1 &&
               CountRecordedMethod(scope.arena, record_path, Str8Lit("exit")) == 1;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
}

TEST(lsp_e2e_real_process_navigation_formatting_and_rename_workspace_edit) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "actions");
  EditorScope editor(scope.arena);

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("proj"));
  String8 bin = OsPathJoin(scope.arena, fixture.path, Str8Lit("bin"));
  CHECK(OsMakeDirs(root));
  CHECK(OsMakeDirs(bin));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, root, Str8Lit(".git")), String8{}));
  (void)InstallClangdAlias(scope.arena, bin);

  String8 record_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("actions.jsonl"));
  String8 main_path = WriteTextFile(scope.arena, root, Str8Lit("main.cpp"), Str8Lit("helper();\n"));
  String8 target_path =
      WriteTextFile(scope.arena, root, Str8Lit("target.cpp"), Str8Lit("void helper(){}\nint spacing=0;\n"));

  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(root));
  ScopedEnvVar path(scope.arena, "PATH", PrependPath(scope.arena, bin));
  ScopedEnvVar mode(scope.arena, kFakeLspModeEnv, Str8Lit("actions"));
  ScopedEnvVar record(scope.arena, kFakeLspRecordEnv, record_path);
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  EditorLspConfig config = RealRegistryConfig();
  EditorLspEnable(&editor.ed, &config);

  Buffer *main_buffer = OpenAndShowFileBuffer(&editor.ed, main_path);
  FocusBuffer(&editor.ed, main_buffer, 0);

  WaitUntilOrFail(
      "main didOpen",
      [&]() {
        (void)EditorTick(&editor.ed);
        return BufferDidOpen(&editor.ed, main_buffer);
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });

  EditorProcessSpec(&editor.ed, "gd");
  WaitUntilOrFail(
      "definition navigation",
      [&]() {
        (void)EditorTick(&editor.ed);
        Buffer *focused = EditorFocusedBuffer(&editor.ed);
        View *view = EditorFocusedView(&editor.ed);
        return focused != nullptr && view != nullptr && Str8Match(focused->path, target_path) &&
               BufferDidOpen(&editor.ed, focused) && ViewCursorLine(view, focused) == 0 &&
               ViewCursorColumn(view, focused) == 5;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/definition")), (u64)1);

  Buffer *target_buffer = EditorFocusedBuffer(&editor.ed);
  CHECK(target_buffer != nullptr);
  CHECK_STR(target_buffer->path, target_path);

  EditorProcessSpec(&editor.ed, "<leader>cf");
  WaitUntilOrFail(
      "formatting result",
      [&]() {
        (void)EditorTick(&editor.ed);
        return Str8Match(BufferTextAll(scope.arena, target_buffer),
                         Str8Lit("void helper(){}\nint spacing = 0;\n"));
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
  CHECK(BufferIsDirty(target_buffer));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/formatting")), (u64)1);

  EditorProcessSpec(&editor.ed, "<leader>rn");
  WaitUntilOrFail(
      "rename prompt",
      [&]() {
        (void)EditorTick(&editor.ed);
        if (!editor.ed.command_line_active) return false;
        Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
        return command != nullptr && Str8Match(BufferTextAll(scope.arena, command), Str8Lit("helper"));
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
  Buffer *command = BufferFromHandle(&editor.ed.buffers, editor.ed.command_buffer);
  CHECK(command != nullptr);
  CHECK_STR(BufferTextAll(scope.arena, command), Str8Lit("helper"));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/prepareRename")), (u64)1);

  EditorProcessSpec(&editor.ed, "<C-w>renamed<CR>");
  WaitUntilOrFail(
      "rename workspace edit",
      [&]() {
        (void)EditorTick(&editor.ed);
        return !editor.ed.command_line_active &&
               Str8Match(BufferTextAll(scope.arena, main_buffer), Str8Lit("renamed();\n")) &&
               Str8Match(BufferTextAll(scope.arena, target_buffer),
                         Str8Lit("void renamed(){}\nint spacing = 0;\n"));
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });

  CHECK(BufferIsDirty(main_buffer));
  CHECK(BufferIsDirty(target_buffer));
  CHECK_EQ(CountRecordedMethod(scope.arena, record_path, Str8Lit("textDocument/rename")), (u64)1);

  EditorLspDisable(&editor.ed);
  WaitUntilOrFail(
      "actions shutdown and exit",
      [&]() {
        return CountRecordedMethod(scope.arena, record_path, Str8Lit("shutdown")) == 1 &&
               CountRecordedMethod(scope.arena, record_path, Str8Lit("exit")) == 1;
      },
      [&]() { return DescribeEditorState(&editor.ed, record_path); });
}

}  // namespace
