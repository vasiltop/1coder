#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "os/os_file.h"
#include "os/os_process.h"
#include "test.h"

#include <string.h>

namespace {

String8 ReadAllStdout(Arena *arena, OsProcess *process) {
  u8 chunk[16];
  String8List parts = {};
  for (;;) {
    OsProcessRead read = OsProcessReadStdout(process, chunk, sizeof(chunk));
    if (read.status == OsProcessReadStatus::Data) {
      Str8ListPush(arena, &parts, PushStr8Copy(arena, Str8(chunk, read.size)));
      continue;
    }
    CHECK(read.status == OsProcessReadStatus::End);
    break;
  }
  return Str8ListJoin(arena, &parts, String8{nullptr, 0});
}

String8 ReadLineFromStderr(Arena *arena, OsProcess *process) {
  u8 buffer[64];
  u64 total = 0;
  for (;;) {
    OsProcessRead read = OsProcessReadStderr(process, buffer + total, sizeof(buffer) - total);
    CHECK(read.status == OsProcessReadStatus::Data);
    total += read.size;
    if (Str8FindFirstChar(Str8(buffer, total), '\n') < total) break;
  }
  return PushStr8Copy(arena, Str8(buffer, total));
}

}  // namespace

TEST(os_process_echo_round_trip_and_waits_cleanly) {
  Arena *arena = ArenaAlloc(MB(1));

  String8 executable = Str8C(EDITOR_FAKE_LSP_PATH);
  String8 arguments[] = {Str8Lit("--echo")};
  OsProcessCommand command = {executable, arguments, ArrayCount(arguments), String8{}};
  OsProcess process = {};

  CHECK(OsProcessStart(&process, &command));

  u8 payload_bytes[] = {'p', 'i', 'n', 'g', 0, 'x', '\n'};
  CHECK(OsProcessWrite(&process, Str8(payload_bytes, ArrayCount(payload_bytes))));
  OsProcessCloseStdin(&process);

  String8 stderr_line = ReadLineFromStderr(arena, &process);
  CHECK_STR(stderr_line, Str8Lit("fake_lsp_server: stderr ready\n"));

  String8 stdout_bytes = ReadAllStdout(arena, &process);
  CHECK_EQ(stdout_bytes.size, (u64)ArrayCount(payload_bytes));
  CHECK_EQ(memcmp(stdout_bytes.str, payload_bytes, ArrayCount(payload_bytes)), 0);

  CHECK_EQ(OsProcessWait(&process), 0);
  OsProcessDestroy(&process);

  ArenaRelease(arena);
}

TEST(os_process_terminate_and_double_destroy_are_safe) {
  String8 arguments[] = {Str8Lit("--sleep")};
  OsProcessCommand command = {Str8C(EDITOR_FAKE_LSP_PATH), arguments, ArrayCount(arguments), String8{}};
  OsProcess process = {};

  CHECK(OsProcessStart(&process, &command));
  OsProcessTerminate(&process);
  CHECK(OsProcessWait(&process) != 0);

  OsProcessDestroy(&process);
  OsProcessDestroy(&process);
}

TEST(os_process_find_executable_handles_absolute_path_and_missing_binary) {
  Arena *arena = ArenaAlloc(MB(1));

  String8 fake_path = Str8C(EDITOR_FAKE_LSP_PATH);
  String8 found = OsFindExecutable(arena, fake_path);
  CHECK(found.size > 0);
  CHECK_STR(found, OsPathAbsolute(arena, fake_path));

  String8 missing = OsFindExecutable(arena, Str8Lit("definitely_missing_1coder_process_test_binary"));
  CHECK_EQ(missing.size, (u64)0);

  ArenaRelease(arena);
}
