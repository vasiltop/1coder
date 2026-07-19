#include "input/keymap.h"

namespace {

[[nodiscard]] KeymapNode *FindChild(KeymapNode *parent, KeyChord chord) {
  for (KeymapNode *child = parent->first_child; child; child = child->next_sibling) {
    if (KeyChordEqual(child->chord, chord)) return child;
  }
  return nullptr;
}

[[nodiscard]] KeymapNode *FindOrAddChild(Arena *arena, KeymapNode *parent, KeyChord chord) {
  KeymapNode *existing = FindChild(parent, chord);
  if (existing) return existing;

  KeymapNode *node = PushStruct(arena, KeymapNode);
  node->chord = chord;
  node->command = CommandId::None;

  // Prepending keeps binding O(1); order within a level never matters because
  // lookup is by exact chord.
  node->next_sibling = parent->first_child;
  parent->first_child = node;
  return node;
}

// Walks a parsed sequence without creating nodes.
[[nodiscard]] KeymapNode *WalkSequence(KeymapNode *root, KeyChordSequence seq) {
  KeymapNode *node = root;
  for (u64 i = 0; i < seq.count && node; i += 1) {
    node = FindChild(node, seq.chords[i]);
  }
  return node;
}

struct EntryCollector {
  Arena *arena;
  KeymapEntry *entries;
  u64 count;
  u64 capacity;
  KeyChord path[kMaxChordSequence];
  u64 depth;
};

void CollectBindings(EntryCollector *c, KeymapNode *node) {
  for (KeymapNode *child = node->first_child; child; child = child->next_sibling) {
    if (c->depth >= kMaxChordSequence) continue;

    c->path[c->depth] = child->chord;
    c->depth += 1;

    if (child->command != CommandId::None && c->count < c->capacity) {
      KeyChordSequence seq = {c->path, c->depth};
      c->entries[c->count].spec = KeyChordSequenceToString(c->arena, seq);
      c->entries[c->count].command = child->command;
      c->count += 1;
    }
    CollectBindings(c, child);

    c->depth -= 1;
  }
}

u64 CountBindings(KeymapNode *node) {
  u64 count = 0;
  for (KeymapNode *child = node->first_child; child; child = child->next_sibling) {
    if (child->command != CommandId::None) count += 1;
    count += CountBindings(child);
  }
  return count;
}

}  // namespace

Keymap *KeymapAlloc(Arena *arena, Keymap *parent, KeyChord leader) {
  Keymap *map = PushStruct(arena, Keymap);
  map->arena = arena;
  map->parent = parent;
  map->leader = leader;
  map->root = PushStruct(arena, KeymapNode);
  map->root->command = CommandId::None;
  return map;
}

bool KeymapBind(Keymap *map, String8 spec, CommandId command) {
  TempArena scratch = ScratchBegin1(map->arena);
  KeyChordSequence seq = KeyChordParseSequence(scratch.arena, spec, map->leader);

  if (seq.count == 0) {
    ScratchEnd(scratch);
    return false;
  }

  KeymapNode *node = map->root;
  for (u64 i = 0; i < seq.count; i += 1) {
    node = FindOrAddChild(map->arena, node, seq.chords[i]);
  }
  node->command = command;

  ScratchEnd(scratch);
  return true;
}

bool KeymapBind(Keymap *map, const char *spec, CommandId command) {
  return KeymapBind(map, Str8C(spec), command);
}

bool KeymapUnbind(Keymap *map, String8 spec) {
  TempArena scratch = ScratchBegin1(map->arena);
  KeyChordSequence seq = KeyChordParseSequence(scratch.arena, spec, map->leader);
  KeymapNode *node = (seq.count > 0) ? WalkSequence(map->root, seq) : nullptr;
  ScratchEnd(scratch);

  if (!node || node->command == CommandId::None) return false;

  // The node itself stays: it may still be an interior node of longer
  // bindings, and orphaned leaves cost only arena space that is freed with the
  // keymap.
  node->command = CommandId::None;
  return true;
}

bool KeymapNodeIsPrefix(const KeymapNode *node) {
  return node && node->first_child != nullptr;
}

KeymapLookup KeymapStep(Keymap *map, KeymapNode *from, KeyChord chord) {
  KeymapLookup result = {};

  if (from) {
    // Mid-sequence: the binding must complete in the map it began in.
    KeymapNode *node = FindChild(from, chord);
    if (node) {
      result.node = node;
      result.command = node->command;
      result.is_prefix = KeymapNodeIsPrefix(node);
      result.found_in = map;
    }
    return result;
  }

  // First chord: try this map, then fall through the parent chain.
  for (Keymap *m = map; m; m = m->parent) {
    KeymapNode *node = FindChild(m->root, chord);
    if (node) {
      result.node = node;
      result.command = node->command;
      result.is_prefix = KeymapNodeIsPrefix(node);
      result.found_in = m;
      return result;
    }
  }

  return result;
}

KeymapLookup KeymapLookupSequence(Keymap *map, String8 spec) {
  KeymapLookup result = {};

  TempArena scratch = ScratchBegin1(map->arena);
  KeyChordSequence seq = KeyChordParseSequence(scratch.arena, spec, map->leader);

  if (seq.count > 0) {
    // Resolve the first chord through the parent chain, then stay in that map.
    result = KeymapStep(map, nullptr, seq.chords[0]);
    for (u64 i = 1; i < seq.count && result.node; i += 1) {
      Keymap *found_in = result.found_in;
      result = KeymapStep(found_in, result.node, seq.chords[i]);
    }
  }

  ScratchEnd(scratch);
  return result;
}

KeymapEntryList KeymapAllBindings(Arena *arena, Keymap *map) {
  EntryCollector collector = {};
  collector.arena = arena;
  collector.capacity = CountBindings(map->root);
  collector.entries = PushArray(arena, KeymapEntry, Max(collector.capacity, (u64)1));

  CollectBindings(&collector, map->root);

  return KeymapEntryList{collector.entries, collector.count};
}
