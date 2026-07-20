#include "os/os_file.h"
#include "test.h"
#include "test_tempdir.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

TEST(os_make_dirs_creates_every_component) {
  TempDir dir = MakeTempDir("mkdirs");

  String8 deep = TempPath(&dir, "a/b/c");
  CHECK(OsMakeDirs(deep));
  CHECK(OsDirExists(deep));
  CHECK(OsDirExists(TempPath(&dir, "a/b")));
  CHECK(OsDirExists(TempPath(&dir, "a")));

  // Applying the same plan twice must not be an error.
  CHECK(OsMakeDirs(deep));
  CHECK(OsMakeDir(deep));

  Destroy(&dir);
}

TEST(os_file_create_refuses_to_clobber) {
  TempDir dir = MakeTempDir("create");

  String8 path = TempPath(&dir, "new.txt");
  CHECK(OsFileCreate(path));
  CHECK(OsFileExists(path));

  FileContents contents = OsFileRead(dir.arena, path);
  CHECK(contents.ok);
  CHECK_EQ(contents.data.size, (u64)0);

  // Creating over something that exists would truncate it.
  CHECK(!OsFileCreate(path));

  // Parents are created on the way, so a typed "sub/deep.txt" line works.
  String8 nested = TempPath(&dir, "sub/deep.txt");
  CHECK(OsFileCreate(nested));
  CHECK(OsFileExists(nested));

  Destroy(&dir);
}

TEST(os_rename_moves_content) {
  TempDir dir = MakeTempDir("rename");

  String8 from = TempPath(&dir, "before.txt");
  String8 to = TempPath(&dir, "nested/after.txt");
  CHECK(OsFileWrite(from, Str8Lit("payload")));

  CHECK(OsRename(from, to));
  CHECK(!OsFileExists(from));
  CHECK(OsFileExists(to));
  CHECK_STR(OsFileRead(dir.arena, to).data, Str8Lit("payload"));

  Destroy(&dir);
}

TEST(os_delete_file_and_empty_dir) {
  TempDir dir = MakeTempDir("delete");

  String8 file = TempPath(&dir, "gone.txt");
  CHECK(OsFileWrite(file, Str8Lit("x")));
  CHECK(OsFileDelete(file));
  CHECK(!OsFileExists(file));

  String8 empty = TempPath(&dir, "empty");
  CHECK(OsMakeDir(empty));
  CHECK(OsDirDelete(empty));
  CHECK(!OsDirExists(empty));

  // rmdir refuses a non-empty directory; that is what the recursive variant is
  // for, and conflating the two would make a typo destructive.
  String8 full = TempPath(&dir, "full");
  CHECK(OsMakeDir(full));
  CHECK(OsFileWrite(OsPathJoin(dir.arena, full, Str8Lit("f.txt")), Str8Lit("x")));
  CHECK(!OsDirDelete(full));
  CHECK(OsDirExists(full));

  Destroy(&dir);
}

TEST(os_delete_never_follows_symlinks) {
  TempDir dir = MakeTempDir("symlink");

  // A tree that links out to a file and a directory the delete must not touch.
  String8 outside_dir = TempPath(&dir, "outside");
  String8 outside_file = TempPath(&dir, "outside/keep.txt");
  CHECK(OsMakeDir(outside_dir));
  CHECK(OsFileWrite(outside_file, Str8Lit("keep me")));

  String8 tree = TempPath(&dir, "tree");
  CHECK(OsMakeDirs(TempPath(&dir, "tree/nested")));
  CHECK(OsFileWrite(TempPath(&dir, "tree/nested/leaf.txt"), Str8Lit("x")));

  TempArena scratch = ScratchBegin1(dir.arena);
  String8 cmd = PushStr8F(scratch.arena, "ln -s '%.*s' '%.*s/link_dir' && ln -s '%.*s' '%.*s/link_file'",
                          (int)outside_dir.size, (char *)outside_dir.str,
                          (int)tree.size, (char *)tree.str,
                          (int)outside_file.size, (char *)outside_file.str,
                          (int)tree.size, (char *)tree.str);
  int rc = system((const char *)cmd.str);
  CHECK_EQ(rc, 0);
  ScratchEnd(scratch);

  // The listing sees the link to a directory as a directory, but knows it is a
  // link -- which is exactly what stops the recursion descending it.
  FileList listed = OsDirList(dir.arena, tree);
  bool saw_link_dir = false;
  for (u64 i = 0; i < listed.count; i += 1) {
    if (!Str8Match(listed.files[i].name, Str8Lit("link_dir"))) continue;
    saw_link_dir = true;
    CHECK(listed.files[i].is_dir);
    CHECK(listed.files[i].is_link);
  }
  CHECK(saw_link_dir);

  CHECK(OsDirDeleteRecursive(tree));
  CHECK(!OsDirExists(tree));

  // The link targets survived.
  CHECK(OsDirExists(outside_dir));
  CHECK(OsFileExists(outside_file));
  CHECK_STR(OsFileRead(dir.arena, outside_file).data, Str8Lit("keep me"));

  Destroy(&dir);
}
