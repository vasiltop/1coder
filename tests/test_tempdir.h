#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "os/os_file.h"

#include <stdlib.h>
#include <unistd.h>

// Tests that touch the real filesystem do it inside one scratch directory that
// is removed at the end. Shared because the os, explorer and image tests all
// need real files -- there is no fake filesystem layer to test against, and
// adding one would only test the fake.

struct TempDir {
  Arena *arena;
  String8 path;
};

// `tag` names the directory, so it must be unique across tests in the process.
inline TempDir MakeTempDir(const char *tag) {
  TempDir dir = {};
  dir.arena = ArenaAlloc(MB(16));

  const char *base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";

  dir.path = PushStr8F(dir.arena, "%s/1code_test_%s_%d", base, tag, (int)getpid());

  TempArena scratch = ScratchBegin1(dir.arena);
  String8 cmd = PushStr8F(scratch.arena, "rm -rf '%.*s' && mkdir -p '%.*s'",
                          (int)dir.path.size, (char *)dir.path.str,
                          (int)dir.path.size, (char *)dir.path.str);
  int rc = system((const char *)cmd.str);
  (void)rc;
  ScratchEnd(scratch);

  return dir;
}

inline void Destroy(TempDir *dir) {
  TempArena scratch = ScratchBegin1(dir->arena);
  String8 cmd = PushStr8F(scratch.arena, "rm -rf '%.*s'", (int)dir->path.size,
                          (char *)dir->path.str);
  int rc = system((const char *)cmd.str);
  (void)rc;
  ScratchEnd(scratch);
  ArenaRelease(dir->arena);
}

// Convenience for building fixtures: `sub/a.txt` relative to the temp dir.
inline String8 TempPath(TempDir *dir, const char *relative) {
  return OsPathJoin(dir->arena, dir->path, Str8C(relative));
}
