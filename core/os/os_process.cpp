#include "os/os_process.h"

#include "os/os_file.h"

#include <stdlib.h>
#include <string.h>

#include <mutex>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

struct OsProcessImpl {
  Arena *arena;
  bool waited;
  bool stdin_closed;
  bool stdout_closed;
  bool stderr_closed;
  i32 exit_code;
#if defined(_WIN32)
  HANDLE process_handle;
  HANDLE thread_handle;
  HANDLE stdin_write;
  HANDLE stdout_read;
  HANDLE stderr_read;
#else
  pid_t pid;
  int stdin_write;
  int stdout_read;
  int stderr_read;
#endif
};

OsProcessImpl *GetImpl(OsProcess *process) {
  return process ? (OsProcessImpl *)process->impl : nullptr;
}

bool IsPathSep(u8 c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

bool HasPathSyntax(String8 path) {
  for (u64 i = 0; i < path.size; i += 1) {
    if (IsPathSep(path.str[i])) return true;
#if defined(_WIN32)
    if (path.str[i] == ':') return true;
#endif
  }
  return false;
}

#if defined(_WIN32)

wchar_t *PushWide(Arena *arena, String8 s) {
  int need = 0;
  if (s.size) {
    need = MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, nullptr, 0);
    if (need <= 0) need = 0;
  }
  wchar_t *out = PushArrayNoZero(arena, wchar_t, (u64)need + 1);
  if (need > 0) {
    MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, out, need);
  }
  out[need] = 0;
  return out;
}

constexpr u64 kWinIoChunk = 0x10000000;

bool HandleIsExecutablePath(String8 path) {
  TempArena scratch = ScratchBegin();
  DWORD attrs = GetFileAttributesW(PushWide(scratch.arena, path));
  ScratchEnd(scratch);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

String8List GetExecutableExtensions(Arena *arena) {
  const char *raw = getenv("PATHEXT");
  String8 path_ext = raw ? Str8C(raw) : Str8Lit(".COM;.EXE;.BAT;.CMD");
  String8List list = {};

  u64 start = 0;
  for (u64 i = 0; i <= path_ext.size; i += 1) {
    bool at_end = (i == path_ext.size);
    if (!at_end && path_ext.str[i] != ';') continue;
    if (i > start) {
      String8 ext = Str8SkipChopWhitespace(Str8Substr(path_ext, RangeU64{start, i}));
      if (ext.size > 0) Str8ListPush(arena, &list, PushStr8Copy(arena, ext));
    }
    start = i + 1;
  }

  if (list.node_count == 0) Str8ListPush(arena, &list, Str8Lit(".EXE"));
  return list;
}

String8 WindowsExecutableCandidate(Arena *arena, String8 base, String8 extension) {
  if (extension.size == 0) return PushStr8Copy(arena, base);
  return PushStr8Cat(arena, base, extension);
}

void AppendBytePiece(Arena *arena, String8List *list, u8 byte) {
  u8 *copy = PushArrayNoZero(arena, u8, 1);
  copy[0] = byte;
  Str8ListPush(arena, list, Str8(copy, 1));
}

void AppendRepeatedByte(Arena *arena, String8List *list, u8 byte, u64 count) {
  if (count == 0) return;
  u8 *copy = PushArrayNoZero(arena, u8, count);
  memset(copy, byte, (size_t)count);
  Str8ListPush(arena, list, Str8(copy, count));
}

String8 TryWindowsExecutable(Arena *arena, String8 base) {
  if (HandleIsExecutablePath(base)) return OsPathAbsolute(arena, base);
  if (Str8PathExt(Str8PathBase(base)).size > 0) return String8{};

  TempArena scratch = ScratchBegin1(arena);
  String8List extensions = GetExecutableExtensions(scratch.arena);
  for (String8Node *node = extensions.first; node; node = node->next) {
    String8 candidate = WindowsExecutableCandidate(scratch.arena, base, node->string);
    if (!HandleIsExecutablePath(candidate)) continue;
    String8 absolute = OsPathAbsolute(arena, candidate);
    ScratchEnd(scratch);
    return absolute;
  }
  ScratchEnd(scratch);
  return String8{};
}

String8 QuoteWindowsArgument(Arena *arena, String8 argument) {
  bool needs_quotes = argument.size == 0;
  for (u64 i = 0; i < argument.size && !needs_quotes; i += 1) {
    u8 c = argument.str[i];
    needs_quotes = CharIsSpace(c) || c == '"';
  }
  if (!needs_quotes) return PushStr8Copy(arena, argument);

  TempArena scratch = ScratchBegin1(arena);
  String8List pieces = {};
  AppendBytePiece(scratch.arena, &pieces, '"');

  u64 slash_count = 0;
  for (u64 i = 0; i < argument.size; i += 1) {
    u8 c = argument.str[i];
    if (c == '\\') {
      slash_count += 1;
      continue;
    }

    if (c == '"') {
      AppendRepeatedByte(scratch.arena, &pieces, '\\', slash_count * 2 + 1);
      AppendBytePiece(scratch.arena, &pieces, '"');
    } else {
      AppendRepeatedByte(scratch.arena, &pieces, '\\', slash_count);
      AppendBytePiece(scratch.arena, &pieces, c);
    }
    slash_count = 0;
  }

  AppendRepeatedByte(scratch.arena, &pieces, '\\', slash_count * 2);
  AppendBytePiece(scratch.arena, &pieces, '"');

  String8 quoted = Str8ListJoin(arena, &pieces, String8{nullptr, 0});
  ScratchEnd(scratch);
  return quoted;
}

bool MakeChildPipe(HANDLE *parent_end, HANDLE *child_end, bool parent_reads) {
  SECURITY_ATTRIBUTES attrs = {};
  attrs.nLength = sizeof(attrs);
  attrs.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &attrs, 0)) return false;

  HANDLE parent = parent_reads ? read_end : write_end;
  if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return false;
  }

  *parent_end = parent;
  *child_end = parent_reads ? write_end : read_end;
  return true;
}

void CloseHandleIfValid(HANDLE *handle) {
  if (*handle && *handle != INVALID_HANDLE_VALUE) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

bool RefreshExitStatus(OsProcessImpl *impl) {
  if (!impl || impl->waited) return true;

  DWORD wait = WaitForSingleObject(impl->process_handle, 0);
  if (wait == WAIT_TIMEOUT) return false;
  if (wait != WAIT_OBJECT_0) return false;

  DWORD exit_code = 1;
  if (!GetExitCodeProcess(impl->process_handle, &exit_code)) exit_code = 1;
  impl->exit_code = (i32)exit_code;
  impl->waited = true;
  return true;
}

#else

bool SetCloseOnExec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool HandleIsExecutablePath(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  struct stat st;
  bool ok = stat(cpath, &st) == 0 && S_ISREG(st.st_mode) && access(cpath, X_OK) == 0;
  ScratchEnd(scratch);
  return ok;
}

void CloseFdIfValid(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

void IgnoreSigpipeOnce() {
  static std::once_flag once;
  std::call_once(once, []() {
    struct sigaction action = {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPIPE, &action, nullptr);
  });
}

bool RefreshExitStatus(OsProcessImpl *impl) {
  if (!impl || impl->waited) return true;

  int status = 0;
  pid_t result = waitpid(impl->pid, &status, WNOHANG);
  if (result == 0) return false;
  if (result < 0) return false;

  if (WIFEXITED(status)) {
    impl->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    impl->exit_code = 128 + WTERMSIG(status);
  } else {
    impl->exit_code = 1;
  }
  impl->waited = true;
  return true;
}

#endif

bool IsRunning(OsProcessImpl *impl) {
  return impl && !RefreshExitStatus(impl);
}

OsProcessImpl *MakeImpl() {
  Arena *arena = ArenaAlloc(KB(64));
  if (!arena) return nullptr;
  OsProcessImpl *impl = PushStruct(arena, OsProcessImpl);
  impl->arena = arena;
#if defined(_WIN32)
  impl->stdin_write = nullptr;
  impl->stdout_read = nullptr;
  impl->stderr_read = nullptr;
#else
  impl->stdin_write = -1;
  impl->stdout_read = -1;
  impl->stderr_read = -1;
#endif
  return impl;
}

String8 TryDirectExecutable(Arena *arena, String8 name) {
#if defined(_WIN32)
  return TryWindowsExecutable(arena, name);
#else
  if (!HandleIsExecutablePath(name)) return String8{};
  return OsPathAbsolute(arena, name);
#endif
}

String8 FindOnPath(Arena *arena, String8 name) {
  const char *raw_path = getenv("PATH");
  if (!raw_path || !*raw_path) return String8{};
  String8 path = Str8C(raw_path);

  TempArena scratch = ScratchBegin1(arena);
#if defined(_WIN32)
  const u8 separator = ';';
#else
  const u8 separator = ':';
#endif

  u64 start = 0;
  for (u64 i = 0; i <= path.size; i += 1) {
    bool at_end = (i == path.size);
    if (!at_end && path.str[i] != separator) continue;

    String8 dir = Str8Substr(path, RangeU64{start, i});
    if (dir.size == 0) dir = Str8Lit(".");
    String8 candidate = OsPathJoin(scratch.arena, dir, name);
    String8 found = TryDirectExecutable(arena, candidate);
    if (found.size > 0) {
      ScratchEnd(scratch);
      return found;
    }
    start = i + 1;
  }

  ScratchEnd(scratch);
  return String8{};
}

void CloseStreams(OsProcessImpl *impl) {
  if (!impl) return;
#if defined(_WIN32)
  if (!impl->stdin_closed) {
    CloseHandleIfValid(&impl->stdin_write);
    impl->stdin_closed = true;
  }
  if (!impl->stdout_closed) {
    CloseHandleIfValid(&impl->stdout_read);
    impl->stdout_closed = true;
  }
  if (!impl->stderr_closed) {
    CloseHandleIfValid(&impl->stderr_read);
    impl->stderr_closed = true;
  }
#else
  if (!impl->stdin_closed) {
    CloseFdIfValid(&impl->stdin_write);
    impl->stdin_closed = true;
  }
  if (!impl->stdout_closed) {
    CloseFdIfValid(&impl->stdout_read);
    impl->stdout_closed = true;
  }
  if (!impl->stderr_closed) {
    CloseFdIfValid(&impl->stderr_read);
    impl->stderr_closed = true;
  }
#endif
}

template <typename CloseFn, typename ReadFn, typename HandleT>
OsProcessRead ReadFromHandleGeneric(bool *closed, HandleT *handle, u8 *buffer, u64 capacity, CloseFn close_fn,
                                    ReadFn read_fn) {
  if (*closed || capacity == 0) return OsProcessRead{OsProcessReadStatus::End, 0};

  for (;;) {
    OsProcessRead read = read_fn(*handle, buffer, capacity);
    if (read.status == OsProcessReadStatus::Error) return read;
    if (read.status == OsProcessReadStatus::End) {
      close_fn(handle);
      *closed = true;
    }
    return read;
  }
}

#if defined(_WIN32)
OsProcessRead ReadHandle(HANDLE handle, u8 *buffer, u64 capacity) {
  DWORD chunk = (DWORD)Min(capacity, (u64)0xFFFFFFFF);
  DWORD got = 0;
  if (ReadFile(handle, buffer, chunk, &got, nullptr)) {
    if (got == 0) return OsProcessRead{OsProcessReadStatus::End, 0};
    return OsProcessRead{OsProcessReadStatus::Data, got};
  }

  DWORD error = GetLastError();
  if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
    return OsProcessRead{OsProcessReadStatus::End, 0};
  }
  return OsProcessRead{OsProcessReadStatus::Error, 0};
}
#else
OsProcessRead ReadFd(int fd, u8 *buffer, u64 capacity) {
  for (;;) {
    ssize_t got = read(fd, buffer, (size_t)capacity);
    if (got > 0) return OsProcessRead{OsProcessReadStatus::Data, (u64)got};
    if (got == 0) return OsProcessRead{OsProcessReadStatus::End, 0};
    if (errno == EINTR) continue;
    return OsProcessRead{OsProcessReadStatus::Error, 0};
  }
}
#endif

}  // namespace

String8 OsFindExecutable(Arena *arena, String8 name) {
  if (!arena || name.size == 0) return String8{};

  if (HasPathSyntax(name)) return TryDirectExecutable(arena, name);
  return FindOnPath(arena, name);
}

bool OsProcessStart(OsProcess *process, const OsProcessCommand *command) {
  if (!process || process->impl || !command || command->executable.size == 0) return false;

#if defined(_WIN32)
  HANDLE stdin_parent = nullptr, stdin_child = nullptr;
  HANDLE stdout_parent = nullptr, stdout_child = nullptr;
  HANDLE stderr_parent = nullptr, stderr_child = nullptr;
  if (!MakeChildPipe(&stdin_parent, &stdin_child, false) ||
      !MakeChildPipe(&stdout_parent, &stdout_child, true) ||
      !MakeChildPipe(&stderr_parent, &stderr_child, true)) {
    CloseHandleIfValid(&stdin_parent);
    CloseHandleIfValid(&stdin_child);
    CloseHandleIfValid(&stdout_parent);
    CloseHandleIfValid(&stdout_child);
    CloseHandleIfValid(&stderr_parent);
    CloseHandleIfValid(&stderr_child);
    return false;
  }

  TempArena scratch = ScratchBegin();
  String8List argv = {};
  Str8ListPush(scratch.arena, &argv, QuoteWindowsArgument(scratch.arena, command->executable));
  for (u64 i = 0; i < command->argument_count; i += 1) {
    Str8ListPush(scratch.arena, &argv, QuoteWindowsArgument(scratch.arena, command->arguments[i]));
  }
  String8 command_line_utf8 = Str8ListJoin(scratch.arena, &argv, Str8Lit(" "));
  wchar_t *command_line = PushWide(scratch.arena, command_line_utf8);
  wchar_t *application = PushWide(scratch.arena, command->executable);
  wchar_t *working_directory =
      command->working_directory.size ? PushWide(scratch.arena, command->working_directory) : nullptr;

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_child;
  startup.hStdOutput = stdout_child;
  startup.hStdError = stderr_child;

  PROCESS_INFORMATION info = {};
  BOOL ok = CreateProcessW(application, command_line, nullptr, nullptr, TRUE, 0, nullptr,
                           working_directory, &startup, &info);
  ScratchEnd(scratch);

  CloseHandleIfValid(&stdin_child);
  CloseHandleIfValid(&stdout_child);
  CloseHandleIfValid(&stderr_child);

  if (!ok) {
    CloseHandleIfValid(&stdin_parent);
    CloseHandleIfValid(&stdout_parent);
    CloseHandleIfValid(&stderr_parent);
    return false;
  }

  OsProcessImpl *impl = MakeImpl();
  if (!impl) {
    CloseHandleIfValid(&stdin_parent);
    CloseHandleIfValid(&stdout_parent);
    CloseHandleIfValid(&stderr_parent);
    TerminateProcess(info.hProcess, 1);
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return false;
  }

  impl->process_handle = info.hProcess;
  impl->thread_handle = info.hThread;
  impl->stdin_write = stdin_parent;
  impl->stdout_read = stdout_parent;
  impl->stderr_read = stderr_parent;
  process->impl = impl;
  return true;
#else
  IgnoreSigpipeOnce();

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    CloseFdIfValid(&stdin_pipe[0]);
    CloseFdIfValid(&stdin_pipe[1]);
    CloseFdIfValid(&stdout_pipe[0]);
    CloseFdIfValid(&stdout_pipe[1]);
    CloseFdIfValid(&stderr_pipe[0]);
    CloseFdIfValid(&stderr_pipe[1]);
    return false;
  }

  bool cloexec_ok = SetCloseOnExec(stdin_pipe[0]) && SetCloseOnExec(stdin_pipe[1]) &&
                    SetCloseOnExec(stdout_pipe[0]) && SetCloseOnExec(stdout_pipe[1]) &&
                    SetCloseOnExec(stderr_pipe[0]) && SetCloseOnExec(stderr_pipe[1]);
  if (!cloexec_ok) {
    CloseFdIfValid(&stdin_pipe[0]);
    CloseFdIfValid(&stdin_pipe[1]);
    CloseFdIfValid(&stdout_pipe[0]);
    CloseFdIfValid(&stdout_pipe[1]);
    CloseFdIfValid(&stderr_pipe[0]);
    CloseFdIfValid(&stderr_pipe[1]);
    return false;
  }

  TempArena scratch = ScratchBegin();
  char **argv = PushArray(scratch.arena, char *, command->argument_count + 2);
  argv[0] = (char *)PushCStr(scratch.arena, command->executable);
  for (u64 i = 0; i < command->argument_count; i += 1) {
    argv[i + 1] = (char *)PushCStr(scratch.arena, command->arguments[i]);
  }
  argv[command->argument_count + 1] = nullptr;
  const char *working_directory =
      command->working_directory.size ? PushCStr(scratch.arena, command->working_directory) : nullptr;

  pid_t pid = fork();
  if (pid == 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    if (dup2(stdin_pipe[0], STDIN_FILENO) < 0 || dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }

    if (stdin_pipe[0] != STDIN_FILENO) close(stdin_pipe[0]);
    if (stdout_pipe[1] != STDOUT_FILENO) close(stdout_pipe[1]);
    if (stderr_pipe[1] != STDERR_FILENO) close(stderr_pipe[1]);

    if (working_directory && chdir(working_directory) != 0) _exit(127);
    execvp(argv[0], argv);
    _exit(127);
  }

  ScratchEnd(scratch);

  if (pid < 0) {
    CloseFdIfValid(&stdin_pipe[0]);
    CloseFdIfValid(&stdin_pipe[1]);
    CloseFdIfValid(&stdout_pipe[0]);
    CloseFdIfValid(&stdout_pipe[1]);
    CloseFdIfValid(&stderr_pipe[0]);
    CloseFdIfValid(&stderr_pipe[1]);
    return false;
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  OsProcessImpl *impl = MakeImpl();
  if (!impl) {
    CloseFdIfValid(&stdin_pipe[1]);
    CloseFdIfValid(&stdout_pipe[0]);
    CloseFdIfValid(&stderr_pipe[0]);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
  }

  impl->pid = pid;
  impl->stdin_write = stdin_pipe[1];
  impl->stdout_read = stdout_pipe[0];
  impl->stderr_read = stderr_pipe[0];
  process->impl = impl;
  return true;
#endif
}

bool OsProcessWrite(OsProcess *process, String8 bytes) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl || impl->stdin_closed) return false;
  if (bytes.size == 0) return true;

#if defined(_WIN32)
  u64 written = 0;
  while (written < bytes.size) {
    DWORD chunk = (DWORD)Min(bytes.size - written, kWinIoChunk);
    DWORD sent = 0;
    if (!WriteFile(impl->stdin_write, bytes.str + written, chunk, &sent, nullptr) || sent == 0) {
      return false;
    }
    written += sent;
  }
  return true;
#else
  u64 written = 0;
  while (written < bytes.size) {
    ssize_t sent = write(impl->stdin_write, bytes.str + written, (size_t)(bytes.size - written));
    if (sent > 0) {
      written += (u64)sent;
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
#endif
}

OsProcessRead OsProcessReadStdout(OsProcess *process, u8 *buffer, u64 capacity) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl) return OsProcessRead{OsProcessReadStatus::End, 0};
#if defined(_WIN32)
  return ReadFromHandleGeneric(&impl->stdout_closed, &impl->stdout_read, buffer, capacity,
                               CloseHandleIfValid, ReadHandle);
#else
  return ReadFromHandleGeneric(&impl->stdout_closed, &impl->stdout_read, buffer, capacity,
                               CloseFdIfValid, ReadFd);
#endif
}

OsProcessRead OsProcessReadStderr(OsProcess *process, u8 *buffer, u64 capacity) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl) return OsProcessRead{OsProcessReadStatus::End, 0};
#if defined(_WIN32)
  return ReadFromHandleGeneric(&impl->stderr_closed, &impl->stderr_read, buffer, capacity,
                               CloseHandleIfValid, ReadHandle);
#else
  return ReadFromHandleGeneric(&impl->stderr_closed, &impl->stderr_read, buffer, capacity,
                               CloseFdIfValid, ReadFd);
#endif
}

void OsProcessCloseStdin(OsProcess *process) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl || impl->stdin_closed) return;

#if defined(_WIN32)
  CloseHandleIfValid(&impl->stdin_write);
#else
  CloseFdIfValid(&impl->stdin_write);
#endif
  impl->stdin_closed = true;
}

void OsProcessTerminate(OsProcess *process) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl || !IsRunning(impl)) return;

#if defined(_WIN32)
  (void)TerminateProcess(impl->process_handle, 1);
#else
  (void)kill(impl->pid, SIGKILL);
#endif
}

i32 OsProcessWait(OsProcess *process) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl) return 0;
  if (impl->waited) return impl->exit_code;

#if defined(_WIN32)
  WaitForSingleObject(impl->process_handle, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(impl->process_handle, &exit_code)) exit_code = 1;
  impl->exit_code = (i32)exit_code;
  impl->waited = true;
#else
  int status = 0;
  for (;;) {
    pid_t result = waitpid(impl->pid, &status, 0);
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) {
      impl->exit_code = 1;
      break;
    }
    if (WIFEXITED(status)) {
      impl->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      impl->exit_code = 128 + WTERMSIG(status);
    } else {
      impl->exit_code = 1;
    }
    break;
  }
  impl->waited = true;
#endif
  return impl->exit_code;
}

void OsProcessDestroy(OsProcess *process) {
  OsProcessImpl *impl = GetImpl(process);
  if (!impl) {
    if (process) *process = {};
    return;
  }

  CloseStreams(impl);
  if (IsRunning(impl)) {
    OsProcessTerminate(process);
    (void)OsProcessWait(process);
  }

#if defined(_WIN32)
  CloseHandleIfValid(&impl->thread_handle);
  CloseHandleIfValid(&impl->process_handle);
#endif

  Arena *arena = impl->arena;
  ArenaRelease(arena);
  *process = {};
}
