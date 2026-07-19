#pragma once

#include "editor/buffer.h"

// Per-window jump list, as in vim: jumpy motions record the position they left,
// and <C-o> / <C-i> walk older / newer entries. The list lives on the View; this
// file is only the data and the walk/push rules.

inline constexpr u64 kJumpListCapacity = 100;

struct JumpEntry {
  BufferHandle buffer;
  u64 offset;
};

struct JumpList {
  JumpEntry entries[kJumpListCapacity];
  u64 count;
  // Points at the entry currently "stood on" while walking, or equals `count`
  // when the cursor is at the live position (not yet an entry).
  u64 index;
};

[[nodiscard]] inline bool JumpEntryEqual(JumpEntry a, JumpEntry b) {
  return BufferHandleEqual(a.buffer, b.buffer) && a.offset == b.offset;
}

// Records `entry` as a jump origin. Truncates any newer entries (a new jump
// after <C-o> drops the forward half), skips a duplicate of the last entry, and
// leaves `index == count` so the live cursor sits past the list tip.
void JumpListPush(JumpList *list, JumpEntry entry);

// Walk one step older / newer. `current` is the live cursor; the first older
// step after a jump saves it so <C-i> can return. `alive` skips closed buffers.
// Returns false when there is nowhere left to go.
using JumpAliveFn = bool (*)(void *ctx, BufferHandle buffer);

[[nodiscard]] bool JumpListOlder(JumpList *list, JumpEntry current, JumpEntry *out,
                                 JumpAliveFn alive, void *ctx);
[[nodiscard]] bool JumpListNewer(JumpList *list, JumpEntry *out, JumpAliveFn alive,
                                 void *ctx);
