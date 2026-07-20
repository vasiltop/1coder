#include "buffers/buf_compile.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "os/os_file.h"
#include "test.h"
#include "test_tempdir.h"

#include <chrono>
#include <thread>

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
  TempDir dir;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  f.dir = MakeTempDir("compile");
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});
  f.ed.cwd = PushStr8Copy(f.arena, f.dir.path);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  Destroy(&f->dir);
  ArenaRelease(f->arena);
}

Buffer *CompileBuffer(Fixture *f) {
  BufferHandle handle = BufferFromName(&f->ed.buffers, Str8Lit("[compile]"));
  return BufferFromHandle(&f->ed.buffers, handle);
}

bool Contains(String8 haystack, String8 needle) {
  return Str8FindFirst(haystack, needle) < haystack.size;
}

// Output lives below the header and its blank line. Matching against the whole
// buffer would accept the command text in the header as a false positive.
bool OutputContains(Buffer *buffer, String8 needle) {
  if (!buffer || BufferLineCount(buffer) < 3) return false;
  TempArena scratch = ScratchBegin();
  u64 start = BufferOffsetFromLine(buffer, 2);
  String8 text = BufferTextRange(scratch.arena, buffer, RangeU64{start, BufferSize(buffer)});
  bool found = Contains(text, needle);
  ScratchEnd(scratch);
  return found;
}

bool WaitForCompileIdle(Fixture *f, int timeout_ms = 5000) {
  auto start = std::chrono::steady_clock::now();
  for (;;) {
    (void)EditorTick(&f->ed);
    Buffer *buffer = CompileBuffer(f);
    if (buffer) {
      TempArena scratch = ScratchBegin();
      String8 text = BufferTextAll(scratch.arena, buffer);
      bool failed = Contains(text, Str8Lit("Failed to start"));
      bool done = Contains(text, Str8Lit("Compilation finished with exit code"));
      ScratchEnd(scratch);
      if (failed) return false;
      if (done) {
        (void)EditorTick(&f->ed);
        return true;
      }
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// A command that prints a unique token and exits 0 on every platform's shell.
constexpr const char *kEchoToken = "compile-token-42";
#if defined(_WIN32)
constexpr const char *kEchoCommand = "echo compile-token-42";
#else
constexpr const char *kEchoCommand = "echo compile-token-42";
#endif

void WriteDiagSources(Fixture *f) {
  CHECK(OsFileWrite(TempPath(&f->dir, "a.cpp"), Str8Lit("int main() {\n  return 0;\n}\n")));
  CHECK(OsFileWrite(TempPath(&f->dir, "b.cpp"), Str8Lit("void f() {\n  int x;\n  int y;\n}\n")));
  CHECK(OsFileWrite(TempPath(&f->dir, "diags.txt"),
                    Str8Lit("a.cpp:1:1: error: first\nb.cpp:2:3: error: second\n")));
}

String8 DiagCatCommand(Fixture *f) {
#if defined(_WIN32)
  return PushStr8F(f->arena, "type diags.txt");
#else
  return PushStr8F(f->arena, "cat diags.txt");
#endif
}

u64 FindCompileLineContaining(Buffer *buffer, String8 needle) {
  for (u64 line = 0; line < BufferLineCount(buffer); line += 1) {
    TempArena scratch = ScratchBegin();
    String8 text = BufferLineText(scratch.arena, buffer, line);
    bool found = Contains(text, needle);
    ScratchEnd(scratch);
    if (found) return line;
  }
  return BufferLineCount(buffer);
}

}  // namespace

TEST(compile_runs_shell_command_into_buffer) {
  Fixture f = MakeFixture();
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 1);

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));

  // First open splits side by side so the source stays visible beside it.
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);
  Buffer *buffer = CompileBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK_EQ((u32)buffer->kind, (u32)BufferKind::Compile);
  CHECK(BufferIsReadOnly(buffer));
  CHECK(EditorFocusedBuffer(&f.ed) == buffer);

  CHECK(OutputContains(buffer, Str8C(kEchoToken)));
  CHECK(OutputContains(buffer, Str8Lit("Compilation finished with exit code 0")));
  CHECK_STR(f.ed.last_compile_command, Str8C(kEchoCommand));

  Destroy(&f);
}

TEST(compile_empty_opens_prompt_with_default) {
  Fixture f = MakeFixture();

  CommandExec(&f.ed, CommandId::compile);
  CHECK(f.ed.command_line_active);

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  String8 line = BufferTextAll(f.arena, command);
  CHECK_STR(line, Str8Lit("compile make -k"));

  Destroy(&f);
}

TEST(recompile_without_history_opens_prompt) {
  Fixture f = MakeFixture();

  CommandExec(&f.ed, CommandId::recompile);
  CHECK(f.ed.command_line_active);

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  String8 line = BufferTextAll(f.arena, command);
  CHECK_STR(line, Str8Lit("compile make -k"));

  Destroy(&f);
}

TEST(recompile_and_leader_rc_rerun_last_command) {
  Fixture f = MakeFixture();

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  CommandExecLine(&f.ed, Str8Lit("recompile"));
  CHECK(WaitForCompileIdle(&f));
  // Already visible -- reuse that window instead of splitting again.
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  Buffer *buffer = CompileBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK(OutputContains(buffer, Str8C(kEchoToken)));
  CHECK(Contains(BufferTextAll(f.arena, buffer),
                 PushStr8F(f.arena, "-*- compile: %s -*-", kEchoCommand)));

  EditorProcessSpec(&f.ed, "<leader>rc");
  CHECK(WaitForCompileIdle(&f));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  CHECK(OutputContains(CompileBuffer(&f), Str8C(kEchoToken)));

  Destroy(&f);
}

TEST(compile_reuses_visible_window_from_other_pane) {
  Fixture f = MakeFixture();

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);

  // Move focus to the other pane (the original scratch), then recompile.
  EditorFocusDir(&f.ed, Dir2::Left);
  CHECK(EditorFocusedBuffer(&f.ed) != CompileBuffer(&f));

  CommandExecLine(&f.ed, Str8Lit("recompile"));
  CHECK(WaitForCompileIdle(&f));
  CHECK_EQ(PanelLeafCount(f.ed.root_panel), 2);
  CHECK(EditorFocusedBuffer(&f.ed) == CompileBuffer(&f));

  Destroy(&f);
}

TEST(compile_empty_prefills_last_command) {
  Fixture f = MakeFixture();

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));

  CommandExec(&f.ed, CommandId::compile);
  CHECK(f.ed.command_line_active);

  Buffer *command = BufferFromHandle(&f.ed.buffers, f.ed.command_buffer);
  String8 line = BufferTextAll(f.arena, command);
  CHECK_STR(line, PushStr8F(f.arena, "compile %s", kEchoCommand));

  Destroy(&f);
}

TEST(compile_next_prev_error_circular) {
  Fixture f = MakeFixture();
  WriteDiagSources(&f);

  String8 cmd = DiagCatCommand(&f);
  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %.*s", (int)cmd.size, (char *)cmd.str));
  CHECK(WaitForCompileIdle(&f));
  CHECK(OutputContains(CompileBuffer(&f), Str8Lit("a.cpp:1:1: error: first")));
  CHECK(OutputContains(CompileBuffer(&f), Str8Lit("b.cpp:2:3: error: second")));

  // First next lands on a.cpp:1:1.
  EditorProcessSpec(&f.ed, "<leader>ne");
  Buffer *focused = EditorFocusedBuffer(&f.ed);
  CHECK(focused != nullptr);
  CHECK(Contains(focused->name, Str8Lit("a.cpp")) || Contains(focused->path, Str8Lit("a.cpp")));
  View *view = EditorFocusedView(&f.ed);
  CHECK_EQ(ViewCursorLine(view, focused), 0);
  CHECK_EQ(ViewCursorColumn(view, focused), 0);

  // Second next advances to b.cpp:2:3.
  EditorProcessSpec(&f.ed, "<leader>ne");
  focused = EditorFocusedBuffer(&f.ed);
  CHECK(Contains(focused->name, Str8Lit("b.cpp")) || Contains(focused->path, Str8Lit("b.cpp")));
  view = EditorFocusedView(&f.ed);
  CHECK_EQ(ViewCursorLine(view, focused), 1);
  CHECK_EQ(ViewCursorColumn(view, focused), 2);

  // From the last error, next wraps to the first.
  EditorProcessSpec(&f.ed, "<leader>ne");
  focused = EditorFocusedBuffer(&f.ed);
  CHECK(Contains(focused->name, Str8Lit("a.cpp")) || Contains(focused->path, Str8Lit("a.cpp")));
  view = EditorFocusedView(&f.ed);
  CHECK_EQ(ViewCursorLine(view, focused), 0);
  CHECK_EQ(ViewCursorColumn(view, focused), 0);

  // From the first, prev wraps to the last.
  EditorProcessSpec(&f.ed, "<leader>pe");
  focused = EditorFocusedBuffer(&f.ed);
  CHECK(Contains(focused->name, Str8Lit("b.cpp")) || Contains(focused->path, Str8Lit("b.cpp")));
  view = EditorFocusedView(&f.ed);
  CHECK_EQ(ViewCursorLine(view, focused), 1);
  CHECK_EQ(ViewCursorColumn(view, focused), 2);

  Destroy(&f);
}

TEST(compile_enter_opens_error_under_cursor) {
  Fixture f = MakeFixture();
  WriteDiagSources(&f);

  String8 cmd = DiagCatCommand(&f);
  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %.*s", (int)cmd.size, (char *)cmd.str));
  CHECK(WaitForCompileIdle(&f));

  Buffer *compile = CompileBuffer(&f);
  CHECK(EditorFocusedBuffer(&f.ed) == compile);

  u64 line = FindCompileLineContaining(compile, Str8Lit("b.cpp:2:3"));
  CHECK(line < BufferLineCount(compile));
  ViewSetCursorLineColumn(EditorFocusedView(&f.ed), compile, line, 0);

  EditorProcessSpec(&f.ed, "<CR>");
  Buffer *focused = EditorFocusedBuffer(&f.ed);
  CHECK(Contains(focused->name, Str8Lit("b.cpp")) || Contains(focused->path, Str8Lit("b.cpp")));
  View *view = EditorFocusedView(&f.ed);
  CHECK_EQ(ViewCursorLine(view, focused), 1);
  CHECK_EQ(ViewCursorColumn(view, focused), 2);

  Destroy(&f);
}

TEST(compile_next_error_with_no_matches_is_noop) {
  Fixture f = MakeFixture();

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));
  CHECK(EditorFocusedBuffer(&f.ed) == CompileBuffer(&f));

  BufferHandle before = EditorFocusedBuffer(&f.ed)->handle;
  u64 cursor = EditorFocusedView(&f.ed)->cursor;

  EditorProcessSpec(&f.ed, "<leader>ne");
  CHECK(BufferHandleEqual(EditorFocusedBuffer(&f.ed)->handle, before));
  CHECK_EQ(EditorFocusedView(&f.ed)->cursor, cursor);

  EditorProcessSpec(&f.ed, "<leader>pe");
  CHECK(BufferHandleEqual(EditorFocusedBuffer(&f.ed)->handle, before));
  CHECK_EQ(EditorFocusedView(&f.ed)->cursor, cursor);

  Destroy(&f);
}
