#include "input/keys.h"
#include "test.h"

namespace {

KeyChordSequence Parse(Arena *arena, const char *spec) {
  return KeyChordParseSequence(arena, Str8C(spec));
}

}  // namespace

TEST(keys_parse_bare_characters) {
  Arena *arena = ArenaAlloc(MB(1));

  KeyChordSequence seq = Parse(arena, "dw");
  CHECK_EQ(seq.count, 2);
  CHECK(KeyChordEqual(seq.chords[0], KeyChordChar('d')));
  CHECK(KeyChordEqual(seq.chords[1], KeyChordChar('w')));

  // Case is significant: `D` is a different binding from `d`, and Shift is
  // carried by the character rather than by a modifier flag.
  KeyChordSequence upper = Parse(arena, "D");
  CHECK(KeyChordEqual(upper.chords[0], KeyChordChar('D')));
  CHECK(!KeyChordEqual(upper.chords[0], KeyChordChar('d')));
  CHECK_EQ((u32)upper.chords[0].mods, (u32)KeyMod::None);

  // Punctuation that vim binds directly.
  KeyChordSequence punct = Parse(arena, "$^{}");
  CHECK_EQ(punct.count, 4);
  CHECK(KeyChordEqual(punct.chords[0], KeyChordChar('$')));
  CHECK(KeyChordEqual(punct.chords[3], KeyChordChar('}')));

  ArenaRelease(arena);
}

TEST(keys_parse_modifiers) {
  Arena *arena = ArenaAlloc(MB(1));

  KeyChordSequence ctrl = Parse(arena, "<C-w>");
  CHECK_EQ(ctrl.count, 1);
  CHECK(KeyChordEqual(ctrl.chords[0], KeyChordKey(Key::W, KeyMod::Ctrl)));
  CHECK(!KeyChordIsChar(ctrl.chords[0]));

  // A modified press is key-based, so it is distinct from the bare character.
  CHECK(!KeyChordEqual(ctrl.chords[0], KeyChordChar('w')));

  CHECK(KeyChordEqual(Parse(arena, "<A-x>").chords[0], KeyChordKey(Key::X, KeyMod::Alt)));
  CHECK(KeyChordEqual(Parse(arena, "<M-x>").chords[0], KeyChordKey(Key::X, KeyMod::Alt)));
  CHECK(KeyChordEqual(Parse(arena, "<D-s>").chords[0], KeyChordKey(Key::S, KeyMod::Super)));

  // Modifiers combine, in any order.
  KeyChord expected = KeyChordKey(Key::A, KeyMod::Ctrl | KeyMod::Shift);
  CHECK(KeyChordEqual(Parse(arena, "<C-S-a>").chords[0], expected));
  CHECK(KeyChordEqual(Parse(arena, "<S-C-a>").chords[0], expected));
  // An uppercase letter under a modifier means the same as an explicit Shift.
  CHECK(KeyChordEqual(Parse(arena, "<C-A>").chords[0], expected));

  ArenaRelease(arena);
}

TEST(keys_parse_named_keys) {
  Arena *arena = ArenaAlloc(MB(1));

  CHECK(KeyChordEqual(Parse(arena, "<Esc>").chords[0], KeyChordKey(Key::Escape)));
  CHECK(KeyChordEqual(Parse(arena, "<Escape>").chords[0], KeyChordKey(Key::Escape)));
  CHECK(KeyChordEqual(Parse(arena, "<CR>").chords[0], KeyChordKey(Key::Return)));
  CHECK(KeyChordEqual(Parse(arena, "<Enter>").chords[0], KeyChordKey(Key::Return)));
  CHECK(KeyChordEqual(Parse(arena, "<Tab>").chords[0], KeyChordKey(Key::Tab)));
  CHECK(KeyChordEqual(Parse(arena, "<BS>").chords[0], KeyChordKey(Key::Backspace)));
  CHECK(KeyChordEqual(Parse(arena, "<Up>").chords[0], KeyChordKey(Key::Up)));
  CHECK(KeyChordEqual(Parse(arena, "<F12>").chords[0], KeyChordKey(Key::F12)));

  // Names are case-insensitive, and combine with modifiers.
  CHECK(KeyChordEqual(Parse(arena, "<esc>").chords[0], KeyChordKey(Key::Escape)));
  CHECK(KeyChordEqual(Parse(arena, "<S-Tab>").chords[0], KeyChordKey(Key::Tab, KeyMod::Shift)));

  ArenaRelease(arena);
}

TEST(keys_parse_leader) {
  Arena *arena = ArenaAlloc(MB(1));

  // The neovim <leader>xy style: three chords, leader plus two characters.
  KeyChordSequence seq = Parse(arena, "<leader>ff");
  CHECK_EQ(seq.count, 3);
  CHECK(KeyChordEqual(seq.chords[0], KeyChordDefaultLeader()));
  CHECK(KeyChordEqual(seq.chords[0], KeyChordChar(' ')));
  CHECK(KeyChordEqual(seq.chords[1], KeyChordChar('f')));
  CHECK(KeyChordEqual(seq.chords[2], KeyChordChar('f')));

  // The leader is configurable, so the same spec can bind to a different key.
  KeyChord comma = KeyChordChar(',');
  KeyChordSequence custom = KeyChordParseSequence(arena, Str8Lit("<leader>w"), comma);
  CHECK_EQ(custom.count, 2);
  CHECK(KeyChordEqual(custom.chords[0], comma));

  ArenaRelease(arena);
}

TEST(keys_parse_mixed_sequences) {
  Arena *arena = ArenaAlloc(MB(1));

  // Window commands mix a modified press with a plain character.
  KeyChordSequence seq = Parse(arena, "<C-w>v");
  CHECK_EQ(seq.count, 2);
  CHECK(KeyChordEqual(seq.chords[0], KeyChordKey(Key::W, KeyMod::Ctrl)));
  CHECK(KeyChordEqual(seq.chords[1], KeyChordChar('v')));

  // Operator + text object.
  CHECK_EQ(Parse(arena, "ciw").count, 3);
  // Longer leader chains.
  CHECK_EQ(Parse(arena, "<leader>gst").count, 4);

  ArenaRelease(arena);
}

TEST(keys_parse_literal_angle_bracket) {
  Arena *arena = ArenaAlloc(MB(1));

  // `<` is a real vim binding (dedent), so a bare '<' must parse as itself.
  KeyChordSequence bare = Parse(arena, "<");
  CHECK_EQ(bare.count, 1);
  CHECK(KeyChordEqual(bare.chords[0], KeyChordChar('<')));

  CHECK(KeyChordEqual(Parse(arena, "<lt>").chords[0], KeyChordChar('<')));
  CHECK(KeyChordEqual(Parse(arena, ">").chords[0], KeyChordChar('>')));

  // An unrecognised <...> form falls back to a literal '<' rather than failing.
  KeyChordSequence junk = Parse(arena, "<nonsense>");
  CHECK(KeyChordEqual(junk.chords[0], KeyChordChar('<')));

  ArenaRelease(arena);
}

TEST(keys_parse_utf8_characters) {
  Arena *arena = ArenaAlloc(MB(1));

  // Bindings on non-ASCII characters, for non-US layouts.
  KeyChordSequence seq = Parse(arena, "\xC3\xA9");
  CHECK_EQ(seq.count, 1);
  CHECK(KeyChordEqual(seq.chords[0], KeyChordChar(0x00E9)));

  ArenaRelease(arena);
}

TEST(keys_parse_empty_and_overlong) {
  Arena *arena = ArenaAlloc(MB(1));

  CHECK_EQ(Parse(arena, "").count, 0);

  // A sequence longer than the limit is rejected rather than truncated.
  char overlong[kMaxChordSequence + 8];
  for (u64 i = 0; i < ArrayCount(overlong) - 1; i += 1) overlong[i] = 'a';
  overlong[ArrayCount(overlong) - 1] = 0;
  KeyChordSequence seq = Parse(arena, overlong);
  CHECK_EQ(seq.count, 0);

  ArenaRelease(arena);
}

TEST(keys_chord_to_string_round_trips) {
  Arena *arena = ArenaAlloc(MB(1));

  const char *specs[] = {
      "d", "D", "$", "<C-w>", "<Esc>", "<CR>", "<Tab>", "<Up>", "<F12>",
      "<A-x>", "<D-s>", "<C-S-a>", "<lt>", "<S-Tab>", "<C-]>",
  };

  for (u64 i = 0; i < ArrayCount(specs); i += 1) {
    KeyChordSequence seq = Parse(arena, specs[i]);
    CHECK_EQ(seq.count, 1);

    String8 text = KeyChordToString(arena, seq.chords[0]);
    KeyChordSequence again = KeyChordParseSequence(arena, text);

    if (again.count != 1 || !KeyChordEqual(again.chords[0], seq.chords[0])) {
      TestFail(__FILE__, __LINE__, "'%s' round-tripped to '%.*s' and did not match", specs[i],
               (int)text.size, (char *)text.str);
    }
  }

  ArenaRelease(arena);
}

TEST(keys_sequence_to_string) {
  Arena *arena = ArenaAlloc(MB(1));

  // What the pending-keys indicator shows while a chord is part-typed.
  CHECK_STR(KeyChordSequenceToString(arena, Parse(arena, "<C-w>v")), Str8Lit("<C-w>v"));
  CHECK_STR(KeyChordSequenceToString(arena, Parse(arena, "ciw")), Str8Lit("ciw"));
  CHECK_STR(KeyChordSequenceToString(arena, Parse(arena, "<leader>ff")), Str8Lit(" ff"));

  ArenaRelease(arena);
}

TEST(keys_names) {
  CHECK_STR(KeyName(Key::Escape), Str8Lit("Esc"));
  CHECK_STR(KeyName(Key::Return), Str8Lit("CR"));
  CHECK_EQ((u32)KeyFromName(Str8Lit("Esc")), (u32)Key::Escape);
  CHECK_EQ((u32)KeyFromName(Str8Lit("escape")), (u32)Key::Escape);
  CHECK_EQ((u32)KeyFromName(Str8Lit("nope")), (u32)Key::Unknown);
  // Letters have no <> name; they print as bare characters.
  CHECK_EQ(KeyName(Key::A).size, 0);
}

TEST(keys_char_chord_normalises_shift) {
  // Shift is expressed by the character, so it must never survive as a flag --
  // otherwise 'D' typed with shift would not match a binding on 'D'.
  KeyChord typed = KeyChordChar('D', KeyMod::Shift);
  CHECK(KeyChordEqual(typed, KeyChordChar('D')));

  // Other modifiers on a character chord are preserved.
  KeyChord alt = KeyChordChar('d', KeyMod::Alt);
  CHECK(HasFlag(alt.mods, KeyMod::Alt));
}
