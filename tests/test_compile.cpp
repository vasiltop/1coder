#include "buffers/buf_compile.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "test.h"

#include <chrono>
#include <thread>

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
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

}  // namespace

TEST(compile_runs_shell_command_into_buffer) {
  Fixture f = MakeFixture();

  CommandExecLine(&f.ed, PushStr8F(f.arena, "compile %s", kEchoCommand));
  CHECK(WaitForCompileIdle(&f));

  Buffer *buffer = CompileBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK_EQ((u32)buffer->kind, (u32)BufferKind::Compile);
  CHECK(BufferIsReadOnly(buffer));

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

  CommandExecLine(&f.ed, Str8Lit("recompile"));
  CHECK(WaitForCompileIdle(&f));

  Buffer *buffer = CompileBuffer(&f);
  CHECK(buffer != nullptr);
  CHECK(OutputContains(buffer, Str8C(kEchoToken)));
  CHECK(Contains(BufferTextAll(f.arena, buffer),
                 PushStr8F(f.arena, "-*- compile: %s -*-", kEchoCommand)));

  EditorProcessSpec(&f.ed, "<leader>rc");
  CHECK(WaitForCompileIdle(&f));

  CHECK(OutputContains(CompileBuffer(&f), Str8C(kEchoToken)));

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
