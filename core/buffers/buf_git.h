#pragma once

#include "buffers/git_ops.h"
#include "editor/buffer.h"

struct Editor;
struct View;

// Magit-inspired git status / log / diff as ordinary BufferKind::Git buffers.
// Status is the hub; log and diff are companion views of the same kind.

[[nodiscard]] BufferHandle GitBufferOpenStatus(Editor *ed);
[[nodiscard]] BufferHandle GitBufferOpenLog(Editor *ed);
[[nodiscard]] BufferHandle GitBufferOpenDiff(Editor *ed, String8 title, String8 body);

void GitBufferReload(Editor *ed, Buffer *buffer);

// Repo root for a git buffer, or empty.
[[nodiscard]] String8 GitBufferRoot(const Buffer *buffer);

// Active Magit-style argument toggles on a status buffer.
[[nodiscard]] GitFlags GitBufferFlags(const Buffer *buffer);
void GitBufferSetFlags(Buffer *buffer, GitFlags flags);

// Cursor helpers used by commands.
[[nodiscard]] bool GitBufferToggleExpand(Editor *ed, Buffer *buffer, View *view);
[[nodiscard]] bool GitBufferStageUnderCursor(Editor *ed, Buffer *buffer, View *view);
[[nodiscard]] bool GitBufferUnstageUnderCursor(Editor *ed, Buffer *buffer, View *view);
[[nodiscard]] bool GitBufferDiscardUnderCursor(Editor *ed, Buffer *buffer, View *view);
[[nodiscard]] bool GitBufferOpenUnderCursor(Editor *ed, Buffer *buffer, View *view);
[[nodiscard]] bool GitBufferDiffUnderCursor(Editor *ed, Buffer *buffer, View *view);

// Destructive discard: stash the pending path, then run after y/N confirm.
void GitBufferRequestDiscard(Editor *ed, Buffer *buffer, View *view);
void GitBufferApplyDiscard(Editor *ed, Buffer *buffer);

[[nodiscard]] bool GitBufferToggleFlag(Editor *ed, Buffer *buffer, char which);
[[nodiscard]] bool GitBufferPull(Editor *ed, Buffer *buffer);
[[nodiscard]] bool GitBufferPush(Editor *ed, Buffer *buffer);
[[nodiscard]] bool GitBufferCommit(Editor *ed, Buffer *buffer, String8 message);
