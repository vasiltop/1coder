#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

struct OsProcessCommand {
  String8 executable;
  String8 *arguments;
  u64 argument_count;
  String8 working_directory;
};

struct OsProcess {
  void *impl;
};

enum class OsProcessReadStatus : u8 { Data, End, Error };

struct OsProcessRead {
  OsProcessReadStatus status;
  u64 size;
};

[[nodiscard]] String8 OsFindExecutable(Arena *arena, String8 name);
[[nodiscard]] bool OsProcessStart(OsProcess *process, const OsProcessCommand *command);
[[nodiscard]] bool OsProcessWrite(OsProcess *process, String8 bytes);
[[nodiscard]] OsProcessRead OsProcessReadStdout(OsProcess *process, u8 *buffer, u64 capacity);
[[nodiscard]] OsProcessRead OsProcessReadStderr(OsProcess *process, u8 *buffer, u64 capacity);
void OsProcessCloseStdin(OsProcess *process);
[[nodiscard]] bool OsProcessHasExited(OsProcess *process);
void OsProcessTerminate(OsProcess *process);
[[nodiscard]] bool OsProcessTryWait(OsProcess *process, i32 *exit_code);
[[nodiscard]] i32 OsProcessWait(OsProcess *process);
void OsProcessDestroy(OsProcess *process);
