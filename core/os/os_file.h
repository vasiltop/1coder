#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Thin file and directory access. Lives in core rather than behind SDL so that
// buffers can be loaded and saved in tests with no window and no SDL link.
//
// Every path handed back uses '/' as the separator, on Windows too: Win32
// accepts forward slashes on input and only emits backslashes, so converting
// once here keeps the rest of the editor from having to know about either.
// Paths coming *in* may use either separator.

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

// A read-only memory mapping of a file.
//
// For large assets this beats reading into an arena: the pages are shared,
// read-only, and only fault in as they are touched. A font collection can run
// to hundreds of megabytes while the handful of tables and outlines actually
// used costs a fraction of that.
//
// The mapping must outlive every pointer taken into it.
struct FileMapping {
  u8 *data;
  u64 size;
  bool ok;
};

[[nodiscard]] FileMapping OsFileMap(String8 path);
void OsFileUnmap(FileMapping *mapping);

[[nodiscard]] bool OsFileExists(String8 path);
[[nodiscard]] bool OsDirExists(String8 path);

struct FileInfo {
  String8 name;  // entry name only, not a full path
  u64 size;
  bool is_dir;
  // A symbolic link. `is_dir` still describes the target, so a link to a
  // directory sorts and opens like one -- but a recursive delete must unlink it
  // rather than descend, or an `rm -rf` could escape the subtree.
  bool is_link;
};

struct FileList {
  FileInfo *files;
  u64 count;
};

// Lists `path`, excluding "." and "..". Entries are sorted directories-first
// then by name, which is the order a file explorer wants to display.
[[nodiscard]] FileList OsDirList(Arena *arena, String8 path);

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
//
// What a file explorer needs to apply an edited listing. Each returns false on
// failure without reporting why: the explorer names the operation that failed
// and there is nothing useful it could do with an errno.

// Creates one level. True if `path` already exists as a directory, so applying
// a plan twice is not an error.
[[nodiscard]] bool OsMakeDir(String8 path);
// Creates every missing component, so "a/b/c" needs no prior "a" or "a/b".
[[nodiscard]] bool OsMakeDirs(String8 path);
// Creates an empty file, including any missing parent directories. False if
// anything already exists at `path` -- creating over a file would truncate it.
[[nodiscard]] bool OsFileCreate(String8 path);
// Moves `from` to `to`, creating `to`'s parent directories. Falls back to a
// copy-and-unlink when the two are on different filesystems.
[[nodiscard]] bool OsRename(String8 from, String8 to);
// Removes a file or a symlink. A link is removed itself, never its target.
[[nodiscard]] bool OsFileDelete(String8 path);
// Removes an empty directory only.
[[nodiscard]] bool OsDirDelete(String8 path);
// Removes a directory and everything under it. Symlinked directories are
// unlinked rather than descended into, so this cannot delete outside `path`.
[[nodiscard]] bool OsDirDeleteRecursive(String8 path);

[[nodiscard]] String8 OsGetCwd(Arena *arena);
// Changes the process working directory. False if `path` cannot be entered.
[[nodiscard]] bool OsSetCwd(String8 path);
// Joins with a separator, avoiding a doubled or missing slash.
[[nodiscard]] String8 OsPathJoin(Arena *arena, String8 a, String8 b);
// Resolves symlinks and "..", returning the input unchanged if that fails.
[[nodiscard]] String8 OsPathAbsolute(Arena *arena, String8 path);
