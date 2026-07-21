#pragma once

#include "base/base_types.h"

struct Editor;

// Live detection of files edited on disk by other programs.
//
// Every open file buffer records the on-disk modification time it was last in
// sync with (Buffer::disk_mtime, set on load and on our own save). These
// functions re-stat those files and reconcile:
//   - a buffer with no unsaved edits is reloaded silently;
//   - a buffer with unsaved edits is flagged (BufferFlags::DiskConflict) and,
//     when it is the focused buffer, the user is asked whether to reload,
//     discarding their changes.
//
// All work happens on the main thread. Nothing here spawns a watcher thread:
// the OS is polled during EditorTick and on window focus, which keeps buffer
// access single-threaded and needs no inotify/FSEvents/kqueue backend.

struct Buffer;

// Reload `buffer` from its file on disk, replacing its contents and discarding
// any unsaved edits, then pull every view onto it back within the new bounds.
// Re-baselines the on-disk stamp and clears any conflict flag (via
// BufferLoadFile). Returns false if the buffer has no path or the read fails.
// Shared by the `:revert` command and the silent auto-reload path.
bool EditorReloadFileBuffer(Editor *ed, Buffer *buffer);

// Rate-limited scan for EditorTick. Cheap to call every tick; does real work at
// most a few times a second (see Editor::file_watch_next_ns).
void EditorFileWatchTick(Editor *ed);

// Immediate, unthrottled scan. Called on window focus-gained so returning to
// the editor reacts at once, and used directly by tests.
void EditorFileWatchScan(Editor *ed);
