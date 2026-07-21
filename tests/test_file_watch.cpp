#include "editor/buffer.h"
#include "editor/buffer_registry.h"
#include "editor/command.h"
#include "editor/editor.h"
#include "editor/file_watch.h"
#include "editor/filetype.h"
#include "input/keys.h"
#include "os/os_file.h"
#include "test.h"
#include "test_tempdir.h"

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
  TempDir dir;
};

Fixture MakeFixture(const char *tag) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  f.dir = MakeTempDir(tag);
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});
  f.ed.cwd = PushStr8Copy(f.arena, f.dir.path);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  Destroy(&f->dir);
  ArenaRelease(f->arena);
}

// Writes a file under the temp dir and opens it into the focused view, the way
// `:edit` would. Returns the live buffer.
Buffer *OpenFile(Fixture *f, const char *rel, String8 contents) {
  String8 path = TempPath(&f->dir, rel);
  CHECK(OsFileWrite(path, contents));
  BufferHandle handle = FiletypeOpen(&f->ed, path);
  CHECK(handle.index != 0);
  EditorShowBuffer(&f->ed, handle);
  return BufferFromHandle(&f->ed.buffers, handle);
}

String8 TextOf(Arena *arena, Buffer *buffer) { return BufferTextAll(arena, buffer); }

// Simulate a change made by another program: overwrite the file, then mark our
// stored stamp stale so the scan reconciles deterministically without depending
// on the filesystem's mtime granularity. A sentinel of 1 can never equal a real
// stamp, and is not 0 (which would read as "the file is gone").
void ExternalWrite(Fixture *f, const char *rel, Buffer *buffer, String8 contents) {
  CHECK(OsFileWrite(TempPath(&f->dir, rel), contents));
  buffer->disk_mtime = 1;
}

}  // namespace

TEST(file_watch_modtime_present_and_absent) {
  Fixture f = MakeFixture("watch_mtime");

  String8 path = TempPath(&f.dir, "a.txt");
  CHECK(OsFileWrite(path, Str8Lit("hi")));
  CHECK(OsFileModTime(path) != 0);
  CHECK_EQ(OsFileModTime(TempPath(&f.dir, "missing.txt")), (u64)0);

  Destroy(&f);
}

TEST(file_watch_reloads_clean_buffer) {
  Fixture f = MakeFixture("watch_clean");
  Buffer *buffer = OpenFile(&f, "a.txt", Str8Lit("hello\n"));
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("hello"));

  ExternalWrite(&f, "a.txt", buffer, Str8Lit("world\n"));
  EditorFileWatchScan(&f.ed);

  // No local edits, so the new contents are adopted silently.
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("world"));
  CHECK(!BufferIsDirty(buffer));
  CHECK(!BufferHasDiskConflict(buffer));
  // The stamp is re-baselined against what was read, not left at the sentinel.
  CHECK(buffer->disk_mtime != 1);
  CHECK_EQ(buffer->disk_mtime, OsFileModTime(TempPath(&f.dir, "a.txt")));

  Destroy(&f);
}

TEST(file_watch_deleted_file_marks_dirty) {
  Fixture f = MakeFixture("watch_deleted");
  Buffer *buffer = OpenFile(&f, "a.txt", Str8Lit("hello\n"));

  CHECK(OsFileDelete(TempPath(&f.dir, "a.txt")));
  EditorFileWatchScan(&f.ed);

  // The text is still ours; marking it dirty is what lets `:w` put it back.
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("hello"));
  CHECK(BufferIsDirty(buffer));
  CHECK(BufferHasDiskConflict(buffer));
  CHECK_EQ(buffer->disk_mtime, (u64)0);

  Destroy(&f);
}

TEST(file_watch_dirty_buffer_prompts_when_focused) {
  Fixture f = MakeFixture("watch_prompt");
  Buffer *buffer = OpenFile(&f, "a.txt", Str8Lit("hello\n"));

  // Unsaved local edits that differ from what is about to land on disk.
  BufferSetText(&f.ed, buffer, Str8Lit("local edit"));
  buffer->flags |= BufferFlags::Dirty;

  ExternalWrite(&f, "a.txt", buffer, Str8Lit("world\n"));
  EditorFileWatchScan(&f.ed);

  // The conflict is flagged and a confirmation is raised against this buffer,
  // but nothing is reloaded until the user answers.
  CHECK(BufferHasDiskConflict(buffer));
  CHECK(f.ed.input.awaiting_confirm);
  CHECK_EQ((u32)f.ed.input.confirm_command, (u32)CommandId::revert);
  CHECK(BufferHandleEqual(f.ed.input.confirm_buffer, buffer->handle));
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("local edit"));

  // Answering `y` runs the revert, discarding the local edits.
  EditorProcessChord(&f.ed, KeyChordChar('y'));
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("world"));
  CHECK(!BufferIsDirty(buffer));
  CHECK(!BufferHasDiskConflict(buffer));

  Destroy(&f);
}

TEST(file_watch_dirty_decline_keeps_edits) {
  Fixture f = MakeFixture("watch_decline");
  Buffer *buffer = OpenFile(&f, "a.txt", Str8Lit("hello\n"));

  BufferSetText(&f.ed, buffer, Str8Lit("local edit"));
  buffer->flags |= BufferFlags::Dirty;

  ExternalWrite(&f, "a.txt", buffer, Str8Lit("world\n"));
  EditorFileWatchScan(&f.ed);
  CHECK(f.ed.input.awaiting_confirm);

  // Any key but `y` cancels: the local edits survive and the file is untouched.
  EditorProcessChord(&f.ed, KeyChordChar('n'));
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK_STR(TextOf(f.arena, buffer), Str8Lit("local edit"));
  CHECK(BufferIsDirty(buffer));
  CHECK(BufferHasDiskConflict(buffer));

  // The stamp was re-baselined, so a re-scan does not nag about the same change.
  EditorFileWatchScan(&f.ed);
  CHECK(!f.ed.input.awaiting_confirm);

  Destroy(&f);
}

TEST(file_watch_save_rebaselines_stamp) {
  Fixture f = MakeFixture("watch_save");
  Buffer *buffer = OpenFile(&f, "a.txt", Str8Lit("hello\n"));

  // A stale stamp stands in for "we have not seen the file since it changed".
  buffer->disk_mtime = 1;
  buffer->flags |= BufferFlags::Dirty;
  CHECK(BufferSaveFile(buffer, String8{}));

  // Our own atomic write must not read back as an external change.
  CHECK(buffer->disk_mtime != 1);
  CHECK_EQ(buffer->disk_mtime, OsFileModTime(TempPath(&f.dir, "a.txt")));

  EditorFileWatchScan(&f.ed);
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK(!BufferHasDiskConflict(buffer));

  Destroy(&f);
}

TEST(file_watch_unfocused_conflict_flags_without_prompt) {
  Fixture f = MakeFixture("watch_unfocused");
  Buffer *a = OpenFile(&f, "a.txt", Str8Lit("aaa\n"));
  Buffer *b = OpenFile(&f, "b.txt", Str8Lit("bbb\n"));

  // Focus A; B is open but in the background.
  EditorShowBuffer(&f.ed, a->handle);
  CHECK(EditorFocusedBuffer(&f.ed) == a);

  BufferSetText(&f.ed, b, Str8Lit("b local"));
  b->flags |= BufferFlags::Dirty;
  ExternalWrite(&f, "b.txt", b, Str8Lit("b disk\n"));
  EditorFileWatchScan(&f.ed);

  // The background buffer is flagged for later, but no prompt hijacks the keys
  // of the buffer the user is actually looking at.
  CHECK(BufferHasDiskConflict(b));
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK_STR(TextOf(f.arena, b), Str8Lit("b local"));

  Destroy(&f);
}
