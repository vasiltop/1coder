#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "os/os_file.h"
#include "os/os_process.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

struct ScopedFixtureDir {
  Arena *arena;
  String8 path;

  ScopedFixtureDir(Arena *arena_, const char *tag) : arena(arena_) {
    static i32 serial = 0;
    serial += 1;
    String8 cwd = OsGetCwd(arena);
    path = PushStr8F(arena, "%.*s/build/test_os_process_%s_%d", (int)cwd.size, (char *)cwd.str, tag,
                     (int)serial);
    (void)OsDirDeleteRecursive(path);
    CHECK(OsMakeDirs(path));
  }

  ~ScopedFixtureDir() { (void)OsDirDeleteRecursive(path); }
};

struct ScopedPathOverride {
  Arena *arena;
  bool had_old_value;
  String8 old_value;

  ScopedPathOverride(Arena *arena_, String8 value) : arena(arena_) {
    const char *old = getenv("PATH");
    had_old_value = old != nullptr;
    if (had_old_value) old_value = PushStr8Copy(arena, Str8C(old));
    Set(value);
  }

  ~ScopedPathOverride() {
    if (had_old_value) {
      Set(old_value);
    } else {
#if defined(_WIN32)
      CHECK(SetEnvironmentVariableA("PATH", nullptr) != 0);
#else
      CHECK(unsetenv("PATH") == 0);
#endif
    }
  }

  void Set(String8 value) {
#if defined(_WIN32)
    CHECK(SetEnvironmentVariableA("PATH", PushCStr(arena, value)) != 0);
#else
    CHECK(setenv("PATH", PushCStr(arena, value), 1) == 0);
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

String8 FakeServerBareName(String8 path) {
  String8 base = Str8PathBase(path);
#if defined(_WIN32)
  String8 ext = Str8PathExt(base);
  if (ext.size > 0) return Str8Prefix(base, base.size - ext.size - 1);
#endif
  return base;
}

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

TEST(os_process_find_executable_uses_path_for_bare_name) {
  Arena *arena = ArenaAlloc(MB(1));

  {
    String8 fake_path = OsPathAbsolute(arena, Str8C(EDITOR_FAKE_LSP_PATH));
    String8 fake_dir = Str8PathDir(fake_path);
    String8 fake_name = FakeServerBareName(fake_path);

    ScopedFixtureDir fixture(arena, "path_lookup");
#if defined(_WIN32)
    String8 local_name = PushStr8Cat(arena, fake_name, Str8Lit(".bat"));
    CHECK(OsFileWrite(OsPathJoin(arena, fixture.path, local_name), Str8Lit("@echo off\r\nexit /b 0\r\n")));
#else
    String8 local_name = PushStr8Copy(arena, fake_name);
    String8 local_path = OsPathJoin(arena, fixture.path, local_name);
    CHECK(OsFileWrite(local_path, Str8Lit("#!/bin/sh\nexit 0\n")));
    CHECK(chmod(PushCStr(arena, local_path), 0777) == 0);
#endif

    ScopedPathOverride path_override(arena, fake_dir);
    ScopedWorkingDirectory cwd_override(arena);
    CHECK(cwd_override.Set(fixture.path));

    String8 found = OsFindExecutable(arena, fake_name);
    CHECK(found.size > 0);
#if defined(_WIN32)
    CHECK(Str8Match(found, fake_path, StringMatch::CaseInsensitive));
#else
    CHECK_STR(found, fake_path);
#endif
  }

  ArenaRelease(arena);
}
