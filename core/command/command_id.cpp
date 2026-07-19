#include "command/command_id.h"

namespace {

const CommandSpec kSpecs[] = {
    {String8{nullptr, 0}, String8{nullptr, 0}, CommandArg::None, CommandFlags::None},  // None
#define X(id, name, desc, arg, flags) {Str8LitComp(name), Str8LitComp(desc), arg, flags},
    COMMAND_LIST
#undef X
};

static_assert(ArrayCount(kSpecs) == (u64)CommandId::COUNT,
              "command spec table must cover every CommandId");

// Short spellings, resolved before prefix matching so that "w" reaches `write`
// even though `write-quit` also starts with it.
struct Alias {
  const char *name;
  CommandId id;
};

const Alias kAliases[] = {
    {"w", CommandId::write_file},   {"q", CommandId::quit},
    {"wq", CommandId::write_quit},  {"x", CommandId::write_quit},
    {"qa", CommandId::quit_all},    {"qall", CommandId::quit_all},
    {"e", CommandId::edit_file},    {"ed", CommandId::edit_file},
    {"b", CommandId::buffer_switch},
    {"bn", CommandId::buffer_next}, {"bp", CommandId::buffer_prev},
    {"vs", CommandId::split_vertical}, {"vsplit", CommandId::split_vertical},
    {"sp", CommandId::split_horizontal}, {"split", CommandId::split_horizontal},
    {"d", CommandId::delete_line},  {"y", CommandId::yank_line},
    {"p", CommandId::paste_after},  {"u", CommandId::undo},
    {"j", CommandId::join_lines},   {"on", CommandId::only_window},
};

// Open-addressed name -> id map, built once. Lookup happens on every command
// submission and every completion keystroke.
constexpr u64 kHashSlots = 512;
static_assert(kHashSlots >= (u64)CommandId::COUNT * 2,
              "hash table must stay sparse enough to avoid clustering");

u16 g_slots[kHashSlots];
bool g_built;

[[nodiscard]] u64 HashName(String8 name) {
  u64 hash = 14695981039346656037ULL;  // FNV-1a
  for (u64 i = 0; i < name.size; i += 1) {
    hash ^= name.str[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void BuildHash() {
  if (g_built) return;

  for (u64 i = 0; i < kHashSlots; i += 1) g_slots[i] = 0;

  for (u64 i = 1; i < (u64)CommandId::COUNT; i += 1) {
    String8 name = kSpecs[i].name;
    u64 slot = HashName(name) % kHashSlots;
    while (g_slots[slot] != 0) {
      // A duplicate name would make one of the two unreachable from the
      // command window, so catch it at startup rather than in the field.
      Assert(!Str8Match(name, kSpecs[g_slots[slot]].name) && "duplicate command name");
      slot = (slot + 1) % kHashSlots;
    }
    g_slots[slot] = (u16)i;
  }

  g_built = true;
}

}  // namespace

const CommandSpec *CommandSpecFromId(CommandId id) {
  if (id >= CommandId::COUNT) return nullptr;
  return &kSpecs[(u64)id];
}

String8 CommandName(CommandId id) {
  const CommandSpec *spec = CommandSpecFromId(id);
  return spec ? spec->name : String8{nullptr, 0};
}

String8 CommandDescription(CommandId id) {
  const CommandSpec *spec = CommandSpecFromId(id);
  return spec ? spec->desc : String8{nullptr, 0};
}

CommandId CommandIdFromName(String8 name) {
  if (name.size == 0) return CommandId::None;
  BuildHash();

  u64 slot = HashName(name) % kHashSlots;
  while (g_slots[slot] != 0) {
    u16 index = g_slots[slot];
    if (Str8Match(name, kSpecs[index].name)) return (CommandId)index;
    slot = (slot + 1) % kHashSlots;
  }
  return CommandId::None;
}

CommandId CommandIdResolve(String8 name) {
  if (name.size == 0) return CommandId::None;

  // 1. An exact name always wins.
  CommandId exact = CommandIdFromName(name);
  if (exact != CommandId::None) return exact;

  // 2. Then the short aliases, which deliberately shadow prefix matching so
  //    that the muscle-memory spellings stay stable as commands are added.
  for (u64 i = 0; i < ArrayCount(kAliases); i += 1) {
    if (Str8Match(name, Str8C(kAliases[i].name))) return kAliases[i].id;
  }

  // 3. Finally an unambiguous prefix. Two candidates means the user has not
  //    said enough yet, so resolve to nothing rather than guessing.
  CommandId found = CommandId::None;
  for (u64 i = 1; i < (u64)CommandId::COUNT; i += 1) {
    if (HasFlag(kSpecs[i].flags, CommandFlags::Hidden)) continue;
    if (!Str8StartsWith(kSpecs[i].name, name)) continue;
    if (found != CommandId::None) return CommandId::None;
    found = (CommandId)i;
  }
  return found;
}

CommandIdList CommandIdsWithPrefix(Arena *arena, String8 prefix, bool include_hidden) {
  CommandIdList list = {};
  list.ids = PushArray(arena, CommandId, (u64)CommandId::COUNT);

  // Declaration order, which groups related commands together in the
  // suggestion list.
  for (u64 i = 1; i < (u64)CommandId::COUNT; i += 1) {
    if (!include_hidden && HasFlag(kSpecs[i].flags, CommandFlags::Hidden)) continue;
    if (!Str8StartsWith(kSpecs[i].name, prefix)) continue;
    list.ids[list.count] = (CommandId)i;
    list.count += 1;
  }
  return list;
}
