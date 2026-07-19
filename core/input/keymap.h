#pragma once

#include "base/base_arena.h"
#include "command/command_id.h"
#include "input/keys.h"

// Keybindings as a trie over chord sequences.
//
// One mechanism covers every binding style the editor needs: a single press
// (`x`), a modified press (`<C-w>`), an operator plus motion (`dw`), a chord
// under a prefix (`<C-w>v`), and the neovim leader style (`<leader>ff`). They
// differ only in depth, so there is no special case for any of them.
//
// Keymaps chain through `parent`. A lookup that finds nothing in a map falls
// through to its parent, which is how buffer-local bindings layer over the
// current vim mode's, which layer over the global ones.

struct KeymapNode {
  KeyChord chord;
  KeymapNode *first_child;
  KeymapNode *next_sibling;
  CommandId command;  // None unless this node terminates a binding
};

struct Keymap {
  KeymapNode *root;
  Keymap *parent;
  KeyChord leader;
  Arena *arena;
};

[[nodiscard]] Keymap *KeymapAlloc(Arena *arena, Keymap *parent = nullptr,
                                  KeyChord leader = KeyChordDefaultLeader());

// Binds a spec such as "dw", "<C-w>v" or "<leader>ff" to a command.
// Returns false if the spec is malformed. Rebinding an existing sequence
// replaces it.
bool KeymapBind(Keymap *map, String8 spec, CommandId command);
bool KeymapBind(Keymap *map, const char *spec, CommandId command);

// Removes a binding. Returns false if nothing was bound there.
bool KeymapUnbind(Keymap *map, String8 spec);

// What a lookup found. A chord sequence can be a complete binding, a prefix of
// longer ones, or both at once (`d` is a prefix of `dw`, and in vim `dd` makes
// `d` meaningful on its own) -- so these are not mutually exclusive.
struct KeymapLookup {
  KeymapNode *node;    // null when nothing matched
  CommandId command;   // None unless the node terminates a binding
  bool is_prefix;      // true when longer bindings extend this sequence
  Keymap *found_in;    // which map in the parent chain matched
};

// Looks up a single chord from a starting node. Pass a null `from` to start at
// the root, which is also the only case that consults the parent chain: once a
// sequence is under way it must complete in the map it started in, or `dw`
// could be finished by a `w` binding from an unrelated map.
[[nodiscard]] KeymapLookup KeymapStep(Keymap *map, KeymapNode *from, KeyChord chord);

// Whole-sequence lookup, mainly for tests and for listing bindings.
[[nodiscard]] KeymapLookup KeymapLookupSequence(Keymap *map, String8 spec);

[[nodiscard]] bool KeymapNodeIsPrefix(const KeymapNode *node);

// Enumerates every binding in the map, deepest-last, for a bindings list.
struct KeymapEntry {
  String8 spec;
  CommandId command;
};
struct KeymapEntryList {
  KeymapEntry *entries;
  u64 count;
};
[[nodiscard]] KeymapEntryList KeymapAllBindings(Arena *arena, Keymap *map);
