#pragma once

#include "base/base_arena.h"
#include "editor/buffer.h"

// Owns every open buffer and hands out generation-tagged handles.
//
// Slots are reused, so a handle alone is not enough to prove a buffer is still
// the one you meant: each slot carries a generation that increments on close,
// and resolving a handle checks it. A view holding a closed buffer's handle
// gets null rather than another buffer's text.

inline constexpr u64 kMaxBuffers = 1024;

struct BufferSlot {
  Buffer buffer;
  u64 generation;
  bool occupied;
};

struct BufferRegistry {
  BufferSlot *slots;
  u64 capacity;
  u64 count;       // occupied slots
  u64 next_free;   // search hint, not a guarantee
  Arena *arena;
};

void BufferRegistryInit(BufferRegistry *reg, Arena *arena, u64 capacity = kMaxBuffers);
void BufferRegistryDestroy(BufferRegistry *reg);

// Returns a zero handle when the registry is full.
[[nodiscard]] BufferHandle BufferOpen(BufferRegistry *reg, BufferKind kind, String8 name);

// Null for a stale or never-valid handle. Always check.
[[nodiscard]] Buffer *BufferFromHandle(BufferRegistry *reg, BufferHandle handle);

void BufferClose(BufferRegistry *reg, Editor *ed, BufferHandle handle);

// Finds an already-open buffer for a path, so opening the same file twice
// shares one buffer rather than creating a second, divergent copy.
[[nodiscard]] BufferHandle BufferFromPath(BufferRegistry *reg, String8 path);
[[nodiscard]] BufferHandle BufferFromName(BufferRegistry *reg, String8 name);

// Iteration in slot order. Pass a zero handle to begin; a zero handle comes
// back when there are no more.
[[nodiscard]] BufferHandle BufferFirst(BufferRegistry *reg);
[[nodiscard]] BufferHandle BufferNext(BufferRegistry *reg, BufferHandle handle);
// Wrapping variants, for :bnext and :bprev.
[[nodiscard]] BufferHandle BufferNextWrapping(BufferRegistry *reg, BufferHandle handle);
[[nodiscard]] BufferHandle BufferPrevWrapping(BufferRegistry *reg, BufferHandle handle);
