#include "os/os_file.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

// Tests touch the real filesystem, so everything happens inside one scratch
// directory that is removed at the end.
struct TempDir {
  Arena *arena;
  String8 path;
};

TempDir MakeTempDir(const char *tag) {
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

void Destroy(TempDir *dir) {
  TempArena scratch = ScratchBegin1(dir->arena);
  String8 cmd = PushStr8F(scratch.arena, "rm -rf '%.*s'", (int)dir->path.size,
                          (char *)dir->path.str);
  int rc = system((const char *)cmd.str);
  (void)rc;
  ScratchEnd(scratch);
  ArenaRelease(dir->arena);
}

}  // namespace

TEST(os_file_write_then_read) {
  TempDir dir = MakeTempDir("rw");
  String8 path = OsPathJoin(dir.arena, dir.path, Str8Lit("hello.txt"));

  CHECK(OsFileWrite(path, Str8Lit("line one\nline two\n")));
  CHECK(OsFileExists(path));

  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(contents.ok);
  CHECK_STR(contents.data, Str8Lit("line one\nline two\n"));
  CHECK_EQ(contents.data.str[contents.data.size], 0);  // null-terminated

  Destroy(&dir);
}

TEST(os_file_overwrite_replaces_contents) {
  TempDir dir = MakeTempDir("overwrite");
  String8 path = OsPathJoin(dir.arena, dir.path, Str8Lit("f.txt"));

  CHECK(OsFileWrite(path, Str8Lit("a much longer original")));
  CHECK(OsFileWrite(path, Str8Lit("short")));

  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(contents.ok);
  CHECK_STR(contents.data, Str8Lit("short"));  // not left with trailing junk

  Destroy(&dir);
}

TEST(os_file_empty_file_reads_ok) {
  TempDir dir = MakeTempDir("empty");
  String8 path = OsPathJoin(dir.arena, dir.path, Str8Lit("empty.txt"));

  CHECK(OsFileWrite(path, String8{nullptr, 0}));

  // An empty file must report ok, distinct from a failed read.
  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(contents.ok);
  CHECK_EQ(contents.data.size, 0);

  Destroy(&dir);
}

TEST(os_file_binary_content_survives) {
  TempDir dir = MakeTempDir("binary");
  String8 path = OsPathJoin(dir.arena, dir.path, Str8Lit("bin"));

  u8 bytes[256];
  for (u32 i = 0; i < 256; i += 1) bytes[i] = (u8)i;

  CHECK(OsFileWrite(path, String8{bytes, 256}));
  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(contents.ok);
  CHECK_EQ(contents.data.size, 256);
  CHECK_EQ(memcmp(contents.data.str, bytes, 256), 0);  // embedded NULs included

  Destroy(&dir);
}

TEST(os_file_missing_file_fails_cleanly) {
  TempDir dir = MakeTempDir("missing");
  String8 path = OsPathJoin(dir.arena, dir.path, Str8Lit("nope.txt"));

  CHECK(!OsFileExists(path));
  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(!contents.ok);
  CHECK_EQ(contents.data.size, 0);

  Destroy(&dir);
}

TEST(os_file_exists_distinguishes_dirs) {
  TempDir dir = MakeTempDir("kinds");
  String8 file = OsPathJoin(dir.arena, dir.path, Str8Lit("f.txt"));
  CHECK(OsFileWrite(file, Str8Lit("x")));

  CHECK(OsFileExists(file));
  CHECK(!OsDirExists(file));
  CHECK(OsDirExists(dir.path));
  CHECK(!OsFileExists(dir.path));

  Destroy(&dir);
}

TEST(os_dir_list_sorts_dirs_first_then_name) {
  TempDir dir = MakeTempDir("list");

  TempArena scratch = ScratchBegin1(dir.arena);
  String8 cmd = PushStr8F(scratch.arena,
                          "cd '%.*s' && touch zebra.txt Apple.txt mango.txt && mkdir -p src docs",
                          (int)dir.path.size, (char *)dir.path.str);
  int rc = system((const char *)cmd.str);
  (void)rc;
  ScratchEnd(scratch);

  FileList list = OsDirList(dir.arena, dir.path);
  CHECK_EQ(list.count, 5);

  // Directories first, then files, each group case-insensitively by name.
  CHECK_STR(list.files[0].name, Str8Lit("docs"));
  CHECK(list.files[0].is_dir);
  CHECK_STR(list.files[1].name, Str8Lit("src"));
  CHECK(list.files[1].is_dir);
  CHECK_STR(list.files[2].name, Str8Lit("Apple.txt"));
  CHECK(!list.files[2].is_dir);
  CHECK_STR(list.files[3].name, Str8Lit("mango.txt"));
  CHECK_STR(list.files[4].name, Str8Lit("zebra.txt"));

  Destroy(&dir);
}

TEST(os_dir_list_empty_and_missing) {
  TempDir dir = MakeTempDir("emptydir");

  FileList empty = OsDirList(dir.arena, dir.path);
  CHECK_EQ(empty.count, 0);

  String8 missing = OsPathJoin(dir.arena, dir.path, Str8Lit("nope"));
  FileList none = OsDirList(dir.arena, missing);
  CHECK_EQ(none.count, 0);

  Destroy(&dir);
}

TEST(os_path_join) {
  Arena *arena = ArenaAlloc(MB(1));

  CHECK_STR(OsPathJoin(arena, Str8Lit("a"), Str8Lit("b")), Str8Lit("a/b"));
  CHECK_STR(OsPathJoin(arena, Str8Lit("a/"), Str8Lit("b")), Str8Lit("a/b"));
  CHECK_STR(OsPathJoin(arena, Str8Lit("a"), Str8Lit("/b")), Str8Lit("a/b"));
  // A separator on both sides must not double up.
  CHECK_STR(OsPathJoin(arena, Str8Lit("a/"), Str8Lit("/b")), Str8Lit("a/b"));
  CHECK_STR(OsPathJoin(arena, Str8Lit(""), Str8Lit("b")), Str8Lit("b"));
  CHECK_STR(OsPathJoin(arena, Str8Lit("a"), Str8Lit("")), Str8Lit("a"));

  ArenaRelease(arena);
}

TEST(os_cwd_and_absolute) {
  Arena *arena = ArenaAlloc(MB(1));

  String8 cwd = OsGetCwd(arena);
  CHECK(cwd.size > 0);
  CHECK_EQ(cwd.str[0], '/');
  CHECK(OsDirExists(cwd));

  CHECK_STR(OsPathAbsolute(arena, Str8Lit(".")), cwd);

  // A path that does not exist cannot be resolved, so it comes back unchanged
  // rather than empty -- callers use this for files about to be created.
  String8 ghost = Str8Lit("/definitely/not/here/file.txt");
  CHECK_STR(OsPathAbsolute(arena, ghost), ghost);

  ArenaRelease(arena);
}
