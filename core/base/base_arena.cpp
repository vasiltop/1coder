#include "base/base_arena.h"

#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace {

u64 PageSize() {
#if defined(_WIN32)
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return (u64)info.dwPageSize;
#else
  static u64 cached = (u64)sysconf(_SC_PAGESIZE);
  return cached;
#endif
}

// Reserve address space without backing it with memory.
u8 *MemReserve(u64 size) {
#if defined(_WIN32)
  return (u8 *)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
  void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? nullptr : (u8 *)p;
#endif
}

bool MemCommit(u8 *base, u64 size) {
#if defined(_WIN32)
  return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
  return mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

void MemRelease(u8 *base, u64 size) {
#if defined(_WIN32)
  (void)size;
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, size);
#endif
}

}  // namespace

Arena *ArenaAlloc(u64 reserve_size) {
  u64 page = PageSize();
  u64 reserve = AlignPow2(Max(reserve_size, kArenaCommitGranularity), page);

  u8 *base = MemReserve(reserve);
  if (!base) return nullptr;

  // The arena header lives in the first slice of its own reservation, so an
  // Arena costs exactly one reservation and one free.
  u64 initial_commit = AlignPow2(Max(kArenaHeaderSize, kArenaCommitGranularity), page);
  if (initial_commit > reserve) initial_commit = reserve;
  if (!MemCommit(base, initial_commit)) {
    MemRelease(base, reserve);
    return nullptr;
  }

  Arena *arena = (Arena *)base;
  arena->base = base;
  arena->capacity = reserve;
  arena->committed = initial_commit;
  arena->pos = kArenaHeaderSize;
  return arena;
}

void ArenaRelease(Arena *arena) {
  if (!arena) return;
  // Copy before unmapping: the header lives inside the range being released.
  u8 *base = arena->base;
  u64 capacity = arena->capacity;
  MemRelease(base, capacity);
}

void *ArenaPush(Arena *arena, u64 size, u64 align) {
  Assert(arena);
  Assert(align > 0 && (align & (align - 1)) == 0);

  u64 start = AlignPow2(arena->pos, align);
  u64 end = start + size;

  if (end > arena->capacity) {
    Assert(!"arena out of reserved space");
    return nullptr;
  }

  if (end > arena->committed) {
    u64 page = PageSize();
    u64 target = AlignPow2(end, Max(kArenaCommitGranularity, page));
    if (target > arena->capacity) target = arena->capacity;
    if (!MemCommit(arena->base + arena->committed, target - arena->committed)) {
      Assert(!"arena commit failed");
      return nullptr;
    }
    arena->committed = target;
  }

  arena->pos = end;
  return arena->base + start;
}

void *ArenaPushZero(Arena *arena, u64 size, u64 align) {
  void *p = ArenaPush(arena, size, align);
  if (p) memset(p, 0, size);
  return p;
}

u64 ArenaPos(Arena *arena) { return arena->pos; }

void ArenaPopTo(Arena *arena, u64 pos) {
  Assert(arena);
  u64 clamped = Max(pos, kArenaHeaderSize);
  if (clamped < arena->pos) arena->pos = clamped;
}

void ArenaPop(Arena *arena, u64 amount) {
  Assert(arena);
  u64 pos = (amount < arena->pos) ? arena->pos - amount : kArenaHeaderSize;
  ArenaPopTo(arena, pos);
}

void ArenaClear(Arena *arena) { ArenaPopTo(arena, kArenaHeaderSize); }

TempArena TempBegin(Arena *arena) { return TempArena{arena, ArenaPos(arena)}; }

void TempEnd(TempArena temp) {
  if (temp.arena) ArenaPopTo(temp.arena, temp.pos);
}

TempArena ScratchBegin(Arena **conflicts, u64 conflict_count) {
  // Four slots. Two covers the common case -- a scratch distinct from the
  // output arena, plus one level of nesting -- but a recursive walk that keeps
  // an accumulating list in one arena while reading directories into another
  // goes deeper than that, and running out is an assert rather than a
  // slowdown. They are allocated on first use and reserve address space only.
  static thread_local Arena *pool[4] = {};

  for (u64 i = 0; i < ArrayCount(pool); i += 1) {
    if (!pool[i]) pool[i] = ArenaAlloc(MB(256));

    bool conflicted = false;
    for (u64 j = 0; j < conflict_count; j += 1) {
      if (conflicts[j] == pool[i]) {
        conflicted = true;
        break;
      }
    }
    if (!conflicted) return TempBegin(pool[i]);
  }

  Assert(!"no non-conflicting scratch arena available");
  return TempArena{nullptr, 0};
}
