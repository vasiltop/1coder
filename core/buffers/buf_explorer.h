#pragma once

#include "editor/buffer.h"

struct Editor;
struct View;

// A directory as a buffer whose text is the listing, in the shape of oil.nvim.
// Editing that text and writing it performs the filesystem operations, so
// renaming a file is `cw`, deleting one is `dd`, and creating one is `o`.
//
// There is one buffer per directory rather than a single explorer that
// retargets. A jump list entry stores a BufferHandle and an offset, so a shared
// buffer would make every historical jump resolve against whatever directory
// happened to be showing. Per-directory buffers also fall straight out of
// BufferFromPath's existing dedupe.

// Opens (or reuses) the explorer for `dir`. The buffer's path is the absolute
// directory, which is what makes the dedupe work.
[[nodiscard]] BufferHandle ExplorerBufferOpen(Editor *ed, String8 dir);

// Rereads the directory from disk. Discards unsaved edits, as reloading must.
void ExplorerBufferReload(Editor *ed, Buffer *buffer);

// Puts the cursor on the line for `name`, or line 0 when it is not listed. This
// is what makes `-` land on the file you came from.
void ExplorerBufferFocusName(Editor *ed, Buffer *buffer, View *view, String8 name);

// The directory an explorer buffer is showing, or empty for any other buffer.
[[nodiscard]] String8 ExplorerBufferDir(const Buffer *buffer);

// The absolute path named by the cursor line, or empty when the line has no id
// yet -- an entry typed but not written does not exist to open.
[[nodiscard]] String8 ExplorerEntryUnderCursor(Arena *arena, Buffer *buffer, View *view);

// Runs the plan that `:w` stashed and the user has just confirmed, then
// reloads. No-op when nothing is pending.
void ExplorerApplyPending(Editor *ed, Buffer *buffer);
