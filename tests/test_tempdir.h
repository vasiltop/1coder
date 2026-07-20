#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "os/os_file.h"

#include <stdlib.h>

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
