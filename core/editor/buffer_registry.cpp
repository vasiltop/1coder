#include "editor/buffer_registry.h"

void BufferRegistryInit(BufferRegistry *reg, Arena *arena, u64 capacity) {
  reg->arena = arena;
  reg->capacity = capacity;
  reg->slots = PushArray(arena, BufferSlot, capacity);
  reg->count = 0;
  // Slot 0 is never handed out, so a zeroed BufferHandle reads as "none".
  reg->next_free = 1;

  for (u64 i = 0; i < capacity; i += 1) reg->slots[i].generation = 1;
}

void BufferRegistryDestroy(BufferRegistry *reg) {
  for (u64 i = 1; i < reg->capacity; i += 1) {
    if (reg->slots[i].occupied) {
      BufferDestroy(&reg->slots[i].buffer);
      reg->slots[i].occupied = false;
    }
  }
  reg->count = 0;
}

BufferHandle BufferOpen(BufferRegistry *reg, BufferKind kind, String8 name) {
  u64 index = 0;
  for (u64 attempt = 1; attempt < reg->capacity; attempt += 1) {
    u64 candidate = reg->next_free + attempt - 1;
    if (candidate >= reg->capacity) candidate = 1 + (candidate - 1) % (reg->capacity - 1);
    if (!reg->slots[candidate].occupied) {
      index = candidate;
      break;
    }
  }
  if (index == 0) return BufferHandleZero();

  BufferSlot *slot = &reg->slots[index];
  slot->occupied = true;
  reg->count += 1;
  reg->next_free = index + 1;

  BufferInit(&slot->buffer, kind, name);
  slot->buffer.handle = BufferHandle{index, slot->generation};
  return slot->buffer.handle;
}

Buffer *BufferFromHandle(BufferRegistry *reg, BufferHandle handle) {
  if (handle.index == 0 || handle.index >= reg->capacity) return nullptr;

  BufferSlot *slot = &reg->slots[handle.index];
  // The generation check is what makes a stale handle safe: the slot may have
  // been reused by an unrelated buffer since.
  if (!slot->occupied || slot->generation != handle.generation) return nullptr;

  return &slot->buffer;
}

void BufferClose(BufferRegistry *reg, Editor *ed, BufferHandle handle) {
  Buffer *buffer = BufferFromHandle(reg, handle);
  if (!buffer) return;

  if (buffer->hooks.on_close) buffer->hooks.on_close(ed, buffer);

  BufferSlot *slot = &reg->slots[handle.index];
  BufferDestroy(&slot->buffer);
  slot->occupied = false;
  // Bumping the generation invalidates every outstanding handle to this slot.
  slot->generation += 1;
  reg->count -= 1;

  if (handle.index < reg->next_free) reg->next_free = handle.index;
}

BufferHandle BufferFromPath(BufferRegistry *reg, String8 path) {
  if (path.size == 0) return BufferHandleZero();

  for (u64 i = 1; i < reg->capacity; i += 1) {
    if (!reg->slots[i].occupied) continue;
    if (Str8Match(reg->slots[i].buffer.path, path)) return reg->slots[i].buffer.handle;
  }
  return BufferHandleZero();
}

BufferHandle BufferFromName(BufferRegistry *reg, String8 name) {
  for (u64 i = 1; i < reg->capacity; i += 1) {
    if (!reg->slots[i].occupied) continue;
    if (Str8Match(reg->slots[i].buffer.name, name)) return reg->slots[i].buffer.handle;
  }
  return BufferHandleZero();
}

BufferHandle BufferFirst(BufferRegistry *reg) {
  for (u64 i = 1; i < reg->capacity; i += 1) {
    if (reg->slots[i].occupied) return reg->slots[i].buffer.handle;
  }
  return BufferHandleZero();
}

BufferHandle BufferNext(BufferRegistry *reg, BufferHandle handle) {
  u64 start = (handle.index == 0) ? 1 : handle.index + 1;
  for (u64 i = start; i < reg->capacity; i += 1) {
    if (reg->slots[i].occupied) return reg->slots[i].buffer.handle;
  }
  return BufferHandleZero();
}

BufferHandle BufferNextWrapping(BufferRegistry *reg, BufferHandle handle) {
  BufferHandle next = BufferNext(reg, handle);
  if (next.index != 0) return next;
  return BufferFirst(reg);
}

BufferHandle BufferPrevWrapping(BufferRegistry *reg, BufferHandle handle) {
  BufferHandle prev = BufferHandleZero();
  for (u64 i = 1; i < reg->capacity; i += 1) {
    if (!reg->slots[i].occupied) continue;
    if (reg->slots[i].buffer.handle.index == handle.index) break;
    prev = reg->slots[i].buffer.handle;
  }
  if (prev.index != 0) return prev;

  // Wrap to the last occupied slot.
  for (u64 i = reg->capacity; i > 1; i -= 1) {
    if (reg->slots[i - 1].occupied) return reg->slots[i - 1].buffer.handle;
  }
  return BufferHandleZero();
}
