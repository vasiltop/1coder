#include "command/command_id.h"

namespace {

struct CommandNameEntry {
  const char *name;
  const char *desc;
};

constexpr CommandNameEntry kCommandNames[] = {
    {"", ""},  // CommandId::None
#define X(id, name, desc) {name, desc},
    COMMAND_LIST
#undef X
};

static_assert(ArrayCount(kCommandNames) == (u64)CommandId::COUNT,
              "command name table must cover every CommandId");

// Open-addressed name -> id map, built once. Command lookup happens on every
// `:` submission and on every completion keystroke, so it should not be a scan
// over the whole table.
constexpr u64 kHashSlots = 512;
static_assert(kHashSlots >= (u64)CommandId::COUNT * 2,
              "hash table must stay sparse enough to avoid clustering");

struct NameHash {
  u16 slots[kHashSlots];
  bool built;
};

NameHash g_name_hash;

[[nodiscard]] u64 HashName(String8 name) {
  u64 hash = 14695981039346656037ULL;  // FNV-1a
  for (u64 i = 0; i < name.size; i += 1) {
    hash ^= name.str[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void BuildHash() {
  if (g_name_hash.built) return;

  for (u64 i = 0; i < kHashSlots; i += 1) g_name_hash.slots[i] = 0;

  for (u64 i = 1; i < (u64)CommandId::COUNT; i += 1) {
    String8 name = Str8C(kCommandNames[i].name);
    u64 slot = HashName(name) % kHashSlots;
    while (g_name_hash.slots[slot] != 0) {
      // A duplicate name would make one of the two commands unreachable from
      // the command line, so catch it at startup rather than in the field.
      Assert(!Str8Match(name, Str8C(kCommandNames[g_name_hash.slots[slot]].name)) &&
             "duplicate command name");
      slot = (slot + 1) % kHashSlots;
    }
    g_name_hash.slots[slot] = (u16)i;
  }

  g_name_hash.built = true;
}

}  // namespace

String8 CommandName(CommandId id) {
  if (id >= CommandId::COUNT) return String8{nullptr, 0};
  return Str8C(kCommandNames[(u64)id].name);
}

String8 CommandDescription(CommandId id) {
  if (id >= CommandId::COUNT) return String8{nullptr, 0};
  return Str8C(kCommandNames[(u64)id].desc);
}

CommandId CommandIdFromName(String8 name) {
  if (name.size == 0) return CommandId::None;
  BuildHash();

  u64 slot = HashName(name) % kHashSlots;
  while (g_name_hash.slots[slot] != 0) {
    u16 index = g_name_hash.slots[slot];
    if (Str8Match(name, Str8C(kCommandNames[index].name))) return (CommandId)index;
    slot = (slot + 1) % kHashSlots;
  }
  return CommandId::None;
}

CommandIdList CommandIdsWithPrefix(Arena *arena, String8 prefix) {
  CommandIdList list = {};
  list.ids = PushArray(arena, CommandId, (u64)CommandId::COUNT);

  // Completion walks the table in declaration order, which groups related
  // commands together in the suggestion list.
  for (u64 i = 1; i < (u64)CommandId::COUNT; i += 1) {
    String8 name = Str8C(kCommandNames[i].name);
    if (Str8StartsWith(name, prefix)) {
      list.ids[list.count] = (CommandId)i;
      list.count += 1;
    }
  }
  return list;
}
