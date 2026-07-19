#include "input/keymap.h"
#include "test.h"

namespace {

// Walks a spec chord by chord the way the input layer will at runtime, rather
// than through the whole-sequence helper.
KeymapLookup StepThrough(Keymap *map, const char *spec) {
  Arena *scratch = ArenaAlloc(MB(1));
  KeyChordSequence seq = KeyChordParseSequence(scratch, Str8C(spec), map->leader);

  KeymapLookup lookup = {};
  if (seq.count > 0) {
    lookup = KeymapStep(map, nullptr, seq.chords[0]);
    for (u64 i = 1; i < seq.count && lookup.node; i += 1) {
      lookup = KeymapStep(lookup.found_in, lookup.node, seq.chords[i]);
    }
  }

  ArenaRelease(scratch);
  return lookup;
}

}  // namespace

TEST(keymap_bind_and_lookup_single_chord) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  CHECK(KeymapBind(map, "x", CommandId::delete_char));

  KeymapLookup hit = KeymapLookupSequence(map, Str8Lit("x"));
  CHECK(hit.node != nullptr);
  CHECK_EQ((u32)hit.command, (u32)CommandId::delete_char);
  CHECK(!hit.is_prefix);

  // An unbound chord finds nothing.
  KeymapLookup miss = KeymapLookupSequence(map, Str8Lit("q"));
  CHECK(miss.node == nullptr);
  CHECK_EQ((u32)miss.command, (u32)CommandId::None);

  ArenaRelease(arena);
}

TEST(keymap_multi_chord_sequences) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  // The four binding shapes the editor needs, through one mechanism.
  CHECK(KeymapBind(map, "dw", CommandId::operator_delete));
  CHECK(KeymapBind(map, "<C-w>v", CommandId::split_vertical));
  CHECK(KeymapBind(map, "<leader>ff", CommandId::edit_file));
  CHECK(KeymapBind(map, "<Esc>", CommandId::normal_mode));

  CHECK_EQ((u32)StepThrough(map, "dw").command, (u32)CommandId::operator_delete);
  CHECK_EQ((u32)StepThrough(map, "<C-w>v").command, (u32)CommandId::split_vertical);
  CHECK_EQ((u32)StepThrough(map, "<leader>ff").command, (u32)CommandId::edit_file);
  CHECK_EQ((u32)StepThrough(map, "<Esc>").command, (u32)CommandId::normal_mode);

  ArenaRelease(arena);
}

TEST(keymap_partial_sequence_is_a_prefix) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  KeymapBind(map, "<leader>ff", CommandId::edit_file);
  KeymapBind(map, "<leader>fg", CommandId::write_file);

  // Partway through, the node exists but resolves to no command yet.
  KeymapLookup partial = StepThrough(map, "<leader>f");
  CHECK(partial.node != nullptr);
  CHECK(partial.is_prefix);
  CHECK_EQ((u32)partial.command, (u32)CommandId::None);

  // A wrong continuation dead-ends rather than falling back.
  CHECK(StepThrough(map, "<leader>fz").node == nullptr);

  CHECK_EQ((u32)StepThrough(map, "<leader>fg").command, (u32)CommandId::write_file);

  ArenaRelease(arena);
}

TEST(keymap_sequence_can_be_both_complete_and_a_prefix) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  // Vim's `d` is exactly this: `dd` deletes a line, and `d` also begins `dw`.
  KeymapBind(map, "d", CommandId::operator_delete);
  KeymapBind(map, "dd", CommandId::delete_line);

  KeymapLookup d = StepThrough(map, "d");
  CHECK_EQ((u32)d.command, (u32)CommandId::operator_delete);
  CHECK(d.is_prefix);  // both at once

  CHECK_EQ((u32)StepThrough(map, "dd").command, (u32)CommandId::delete_line);

  ArenaRelease(arena);
}

TEST(keymap_rebinding_replaces) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  KeymapBind(map, "x", CommandId::delete_char);
  KeymapBind(map, "x", CommandId::undo);

  CHECK_EQ((u32)KeymapLookupSequence(map, Str8Lit("x")).command, (u32)CommandId::undo);
  CHECK_EQ(KeymapAllBindings(arena, map).count, 1);  // replaced, not duplicated

  ArenaRelease(arena);
}

TEST(keymap_unbind) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  KeymapBind(map, "dw", CommandId::operator_delete);
  KeymapBind(map, "d", CommandId::operator_delete);

  CHECK(KeymapUnbind(map, Str8Lit("d")));
  CHECK_EQ((u32)StepThrough(map, "d").command, (u32)CommandId::None);
  // Removing `d` must not orphan the longer binding that runs through it.
  CHECK_EQ((u32)StepThrough(map, "dw").command, (u32)CommandId::operator_delete);

  CHECK(!KeymapUnbind(map, Str8Lit("d")));      // already gone
  CHECK(!KeymapUnbind(map, Str8Lit("zzz")));    // never bound

  ArenaRelease(arena);
}

TEST(keymap_rejects_malformed_specs) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  CHECK(!KeymapBind(map, "", CommandId::undo));

  // Longer than the chord-sequence limit.
  char overlong[kMaxChordSequence + 8];
  for (u64 i = 0; i < ArrayCount(overlong) - 1; i += 1) overlong[i] = 'a';
  overlong[ArrayCount(overlong) - 1] = 0;
  CHECK(!KeymapBind(map, overlong, CommandId::undo));

  ArenaRelease(arena);
}

TEST(keymap_parent_chain_fallback) {
  Arena *arena = ArenaAlloc(MB(1));

  Keymap *global = KeymapAlloc(arena);
  Keymap *mode = KeymapAlloc(arena, global);
  Keymap *buffer_local = KeymapAlloc(arena, mode);

  KeymapBind(global, "<C-w>v", CommandId::split_vertical);
  KeymapBind(mode, "x", CommandId::delete_char);
  KeymapBind(buffer_local, "q", CommandId::quit);

  // A lookup from the innermost map reaches every level.
  CHECK_EQ((u32)StepThrough(buffer_local, "q").command, (u32)CommandId::quit);
  CHECK_EQ((u32)StepThrough(buffer_local, "x").command, (u32)CommandId::delete_char);
  CHECK_EQ((u32)StepThrough(buffer_local, "<C-w>v").command, (u32)CommandId::split_vertical);

  // Fallback goes one way only.
  CHECK(StepThrough(global, "q").node == nullptr);

  ArenaRelease(arena);
}

TEST(keymap_inner_map_shadows_outer) {
  Arena *arena = ArenaAlloc(MB(1));

  Keymap *global = KeymapAlloc(arena);
  Keymap *local = KeymapAlloc(arena, global);

  KeymapBind(global, "x", CommandId::delete_char);
  KeymapBind(local, "x", CommandId::quit);

  CHECK_EQ((u32)StepThrough(local, "x").command, (u32)CommandId::quit);
  // The shadowed binding is untouched in its own map.
  CHECK_EQ((u32)StepThrough(global, "x").command, (u32)CommandId::delete_char);

  ArenaRelease(arena);
}

TEST(keymap_sequence_completes_in_the_map_it_started_in) {
  Arena *arena = ArenaAlloc(MB(1));

  Keymap *global = KeymapAlloc(arena);
  Keymap *local = KeymapAlloc(arena, global);

  // `d` begins a sequence in the outer map; the inner map binds `w` alone.
  KeymapBind(global, "dw", CommandId::operator_delete);
  KeymapBind(local, "w", CommandId::word_forward);

  // Typing `dw` must reach delete-word, not be hijacked partway by the inner
  // map's `w`.
  CHECK_EQ((u32)StepThrough(local, "dw").command, (u32)CommandId::operator_delete);
  CHECK_EQ((u32)StepThrough(local, "w").command, (u32)CommandId::word_forward);

  ArenaRelease(arena);
}

TEST(keymap_found_in_reports_the_matching_map) {
  Arena *arena = ArenaAlloc(MB(1));

  Keymap *global = KeymapAlloc(arena);
  Keymap *local = KeymapAlloc(arena, global);

  KeymapBind(global, "g", CommandId::file_start);
  KeymapBind(local, "l", CommandId::cursor_right);

  CHECK(StepThrough(local, "g").found_in == global);
  CHECK(StepThrough(local, "l").found_in == local);

  ArenaRelease(arena);
}

TEST(keymap_custom_leader) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena, nullptr, KeyChordChar(','));

  KeymapBind(map, "<leader>w", CommandId::write_file);

  // The leader resolves through the map's own configuration.
  KeymapLookup lookup = KeymapStep(map, nullptr, KeyChordChar(','));
  CHECK(lookup.node != nullptr);
  lookup = KeymapStep(map, lookup.node, KeyChordChar('w'));
  CHECK_EQ((u32)lookup.command, (u32)CommandId::write_file);

  // Space is not the leader here.
  CHECK(KeymapStep(map, nullptr, KeyChordChar(' ')).node == nullptr);

  ArenaRelease(arena);
}

TEST(keymap_distinguishes_char_and_modified_chords) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  KeymapBind(map, "d", CommandId::operator_delete);
  KeymapBind(map, "<C-d>", CommandId::scroll_half_page_down);
  KeymapBind(map, "D", CommandId::delete_to_line_end);

  CHECK_EQ((u32)KeymapStep(map, nullptr, KeyChordChar('d')).command,
           (u32)CommandId::operator_delete);
  CHECK_EQ((u32)KeymapStep(map, nullptr, KeyChordKey(Key::D, KeyMod::Ctrl)).command,
           (u32)CommandId::scroll_half_page_down);
  CHECK_EQ((u32)KeymapStep(map, nullptr, KeyChordChar('D')).command,
           (u32)CommandId::delete_to_line_end);

  // Shift arriving as a flag on a character press must still match 'D'.
  CHECK_EQ((u32)KeymapStep(map, nullptr, KeyChordChar('D', KeyMod::Shift)).command,
           (u32)CommandId::delete_to_line_end);

  ArenaRelease(arena);
}

TEST(keymap_all_bindings) {
  Arena *arena = ArenaAlloc(MB(1));
  Keymap *map = KeymapAlloc(arena);

  KeymapBind(map, "x", CommandId::delete_char);
  KeymapBind(map, "dw", CommandId::operator_delete);
  KeymapBind(map, "<leader>ff", CommandId::edit_file);

  KeymapEntryList list = KeymapAllBindings(arena, map);
  CHECK_EQ(list.count, 3);

  // Every listed spec must parse back to the binding it came from.
  for (u64 i = 0; i < list.count; i += 1) {
    KeymapLookup lookup = KeymapLookupSequence(map, list.entries[i].spec);
    CHECK_EQ((u32)lookup.command, (u32)list.entries[i].command);
  }

  ArenaRelease(arena);
}

TEST(command_id_name_lookup) {
  CHECK_STR(CommandName(CommandId::write_file), Str8Lit("write"));
  CHECK_STR(CommandName(CommandId::split_vertical), Str8Lit("split-vertical"));
  CHECK_EQ(CommandName(CommandId::None).size, 0);

  CHECK_EQ((u32)CommandIdFromName(Str8Lit("write")), (u32)CommandId::write_file);
  CHECK_EQ((u32)CommandIdFromName(Str8Lit("quit")), (u32)CommandId::quit);
  CHECK_EQ((u32)CommandIdFromName(Str8Lit("nonesuch")), (u32)CommandId::None);
  CHECK_EQ((u32)CommandIdFromName(Str8Lit("")), (u32)CommandId::None);

  // Every command must be reachable by name from the `:` prompt.
  for (u16 i = 1; i < (u16)CommandId::COUNT; i += 1) {
    CommandId id = (CommandId)i;
    String8 name = CommandName(id);
    CHECK(name.size > 0);
    CHECK_EQ((u32)CommandIdFromName(name), (u32)id);
    CHECK(CommandDescription(id).size > 0);
  }
}

TEST(command_id_prefix_completion) {
  Arena *arena = ArenaAlloc(MB(1));

  CommandIdList splits = CommandIdsWithPrefix(arena, Str8Lit("split-"));
  CHECK_EQ(splits.count, 2);

  CommandIdList write = CommandIdsWithPrefix(arena, Str8Lit("write"));
  CHECK_EQ(write.count, 2);  // write, write-quit

  CHECK_EQ(CommandIdsWithPrefix(arena, Str8Lit("zzz")).count, 0);

  // Keybinding-only actions are hidden from completion, so an empty prefix
  // offers the typeable commands rather than all 80-odd of them.
  u64 visible = CommandIdsWithPrefix(arena, Str8Lit("")).count;
  u64 all = CommandIdsWithPrefix(arena, Str8Lit(""), true).count;
  CHECK(visible > 0);
  CHECK(visible < all);
  CHECK_EQ(all, (u64)CommandId::COUNT - 1);

  // A hidden command is still reachable by exact name, so a binding or a
  // future macro can call it.
  CHECK_EQ((u32)CommandIdFromName(Str8Lit("cursor-left")), (u32)CommandId::cursor_left);

  ArenaRelease(arena);
}
