#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "os/os_file.h"

#include <stdlib.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

// Tests that touch the real filesystem do it inside one scratch directory that
// is removed at the end. Shared because the os, explorer and image tests all
// need real files -- there is no fake filesystem layer to test against, and
// adding one would only test the fake.
//
// Built through the os layer rather than by shelling out to mkdir and rm, so
// the fixtures run wherever the editor does.

struct TempDir {
  Arena *arena;
  String8 path;
};

// Where the platform puts scratch files. TMPDIR is the POSIX spelling; Windows
// sets TEMP and TMP instead.
inline String8 TempBaseDir(Arena *arena) {
  const char *names[] = {"TMPDIR", "TEMP", "TMP"};
  for (const char *name : names) {
    const char *value = getenv(name);
    if (value && *value) return PushStr8Copy(arena, Str8C(value));
  }
#if defined(_WIN32)
  return PushStr8Copy(arena, Str8Lit("C:/Windows/Temp"));
#else
  return PushStr8Copy(arena, Str8Lit("/tmp"));
#endif
}

// `tag` names the directory, so it must be unique across tests in the process.
// The counter keeps repeat calls with one tag apart, which getpid did not.
inline TempDir MakeTempDir(const char *tag) {
  static int serial = 0;
  serial += 1;

  TempDir dir = {};
  dir.arena = ArenaAlloc(MB(16));

  String8 base = TempBaseDir(dir.arena);
  dir.path =
      PushStr8F(dir.arena, "%.*s/1code_test_%s_%d", (int)base.size, (char *)base.str, tag, serial);

  // A run that died before its teardown would otherwise leave entries behind
  // for this one to trip over.
  (void)OsDirDeleteRecursive(dir.path);
  (void)OsMakeDirs(dir.path);

  return dir;
}

inline void Destroy(TempDir *dir) {
  (void)OsDirDeleteRecursive(dir->path);
  ArenaRelease(dir->arena);
}

// Convenience for building fixtures: `sub/a.txt` relative to the temp dir.
inline String8 TempPath(TempDir *dir, const char *relative) {
  return OsPathJoin(dir->arena, dir->path, Str8C(relative));
}

// Creates a symbolic link, which the os layer itself has no reason to expose --
// the editor reads links but never makes them, so this lives with the fixtures.
//
// False means the platform would not create it. On Windows that is the usual
// case rather than an error: an unprivileged process cannot make symlinks
// without Developer Mode, so callers skip rather than fail.
inline bool MakeSymlink(String8 target, String8 link, bool target_is_dir) {
#if defined(_WIN32)
  TempArena scratch = ScratchBegin();
  // Wide strings, for the same reason the os layer uses them.
  auto push_wide = [](Arena *arena, String8 s) {
    int need = MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, nullptr, 0);
    if (need < 0) need = 0;
    wchar_t *out = PushArrayNoZero(arena, wchar_t, (u64)need + 1);
    if (need > 0) MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, out, need);
    out[need] = 0;
    return out;
  };
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (target_is_dir) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  bool ok = CreateSymbolicLinkW(push_wide(scratch.arena, link), push_wide(scratch.arena, target),
                                flags) != 0;
  ScratchEnd(scratch);
  return ok;
#else
  (void)target_is_dir;
  TempArena scratch = ScratchBegin();
  bool ok = symlink(PushCStr(scratch.arena, target), PushCStr(scratch.arena, link)) == 0;
  ScratchEnd(scratch);
  return ok;
#endif
}
