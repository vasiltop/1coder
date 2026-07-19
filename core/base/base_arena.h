#pragma once

#include "base/base_types.h"

// Linear bump allocator over a large reserved virtual range, committed on
// demand. Every subsystem takes an Arena*; nothing in the editor calls malloc.
// Freeing is done by popping back to a saved position or releasing the whole
// arena, which is what makes teardown in tests trivially leak-free.

struct Arena {
  u8 *base;        // start of the reserved range
  u64 capacity;    // total reserved bytes
  u64 committed;   // bytes committed so far
  u64 pos;         // current allocation offset from base
};

inline constexpr u64 kArenaDefaultReserve = GB(1);
inline constexpr u64 kArenaCommitGranularity = KB(64);
inline constexpr u64 kArenaHeaderSize = 128;
inline constexpr u64 kArenaDefaultAlign = 16;

[[nodiscard]] Arena *ArenaAlloc(u64 reserve_size = kArenaDefaultReserve);
void ArenaRelease(Arena *arena);

[[nodiscard]] void *ArenaPush(Arena *arena, u64 size, u64 align = kArenaDefaultAlign);
[[nodiscard]] void *ArenaPushZero(Arena *arena, u64 size, u64 align = kArenaDefaultAlign);

[[nodiscard]] u64 ArenaPos(Arena *arena);
void ArenaPopTo(Arena *arena, u64 pos);
void ArenaPop(Arena *arena, u64 amount);
void ArenaClear(Arena *arena);

#define PushArrayNoZero(a, T, c) (T *)ArenaPush((a), sizeof(T) * (u64)(c), alignof(T))
#define PushArray(a, T, c) (T *)ArenaPushZero((a), sizeof(T) * (u64)(c), alignof(T))
#define PushStruct(a, T) PushArray(a, T, 1)

// Save/restore point for temporary allocations within a scope.
struct TempArena {
  Arena *arena;
  u64 pos;
};

[[nodiscard]] TempArena TempBegin(Arena *arena);
void TempEnd(TempArena temp);

// Thread-local scratch arenas for short-lived intermediate results. Pass any
// arena you are writing the *result* into as a conflict, so the scratch handed
// back is never the same arena you are building into.
[[nodiscard]] TempArena ScratchBegin(Arena **conflicts = nullptr, u64 conflict_count = 0);
inline void ScratchEnd(TempArena temp) { TempEnd(temp); }

[[nodiscard]] inline TempArena ScratchBegin1(Arena *conflict) {
  return ScratchBegin(&conflict, 1);
}
