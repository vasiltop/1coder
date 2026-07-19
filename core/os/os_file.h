#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Thin file and directory access. Lives in core rather than behind SDL so that
// buffers can be loaded and saved in tests with no window and no SDL link.

struct FileContents {
  String8 data;
  bool ok;  // distinguishes "read an empty file" from "failed to read"
};

// Reads the whole file into `arena`. The result is null-terminated past `size`.
[[nodiscard]] FileContents OsFileRead(Arena *arena, String8 path);

// Writes atomically where the platform allows: content goes to a sibling
// temporary file which is then renamed over the target, so an interrupted save
// cannot truncate the original.
[[nodiscard]] bool OsFileWrite(String8 path, String8 data);

[[nodiscard]] bool OsFileExists(String8 path);
[[nodiscard]] bool OsDirExists(String8 path);

struct FileInfo {
  String8 name;  // entry name only, not a full path
  u64 size;
  bool is_dir;
};

struct FileList {
  FileInfo *files;
  u64 count;
};

// Lists `path`, excluding "." and "..". Entries are sorted directories-first
// then by name, which is the order a file explorer wants to display.
[[nodiscard]] FileList OsDirList(Arena *arena, String8 path);

[[nodiscard]] String8 OsGetCwd(Arena *arena);
// Joins with a separator, avoiding a doubled or missing slash.
[[nodiscard]] String8 OsPathJoin(Arena *arena, String8 a, String8 b);
// Resolves symlinks and "..", returning the input unchanged if that fails.
[[nodiscard]] String8 OsPathAbsolute(Arena *arena, String8 path);
