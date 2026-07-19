#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Platform-agnostic key identity. Nothing here knows about SDL; the platform
// layer translates into these types, which is what lets the whole input and
// vim stack be driven from tests with no window.

enum class Key : u16 {
  Unknown = 0,

  A, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

  Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

  Escape, Return, Tab, Space, Backspace, Delete, Insert,
  Left, Right, Up, Down, Home, End, PageUp, PageDown,

  Minus, Equal, LeftBracket, RightBracket, Backslash,
  Semicolon, Apostrophe, Grave, Comma, Period, Slash,

  COUNT
};

enum class KeyMod : u8 {
  None = 0,
  Ctrl = 1,
  Shift = 2,
  Alt = 4,
  Super = 8,
};
ENUM_FLAG_OPS(KeyMod)

// A single press in a binding.
//
// Bindings come in two flavours, because vim's are character-based while
// control combinations are key-based:
//
//   character chord -- `d`, `D`, `$`, `{`. `codepoint` is set, `key` is
//                      Unknown, and Shift is *not* recorded: it is already
//                      expressed by the character itself ('D' vs 'd').
//   key chord       -- `<C-w>`, `<Esc>`, `<Up>`, `<S-Tab>`. `key` is set,
//                      `codepoint` is 0, and mods are recorded literally.
//
// Keeping these distinct is what makes `d` and `<C-d>` different bindings while
// `$` still works on any keyboard layout that can produce it.
struct KeyChord {
  Key key;
  u32 codepoint;
  KeyMod mods;
};

[[nodiscard]] inline KeyChord KeyChordChar(u32 codepoint, KeyMod mods = KeyMod::None) {
  // Shift is implied by the character, so drop it to keep chords comparable.
  return KeyChord{Key::Unknown, codepoint, mods & ~KeyMod::Shift};
}

[[nodiscard]] inline KeyChord KeyChordKey(Key key, KeyMod mods = KeyMod::None) {
  return KeyChord{key, 0, mods};
}

[[nodiscard]] inline bool KeyChordIsChar(KeyChord c) { return c.codepoint != 0; }

[[nodiscard]] inline bool KeyChordValid(KeyChord c) {
  return c.codepoint != 0 || c.key != Key::Unknown;
}

[[nodiscard]] inline bool KeyChordEqual(KeyChord a, KeyChord b) {
  return a.key == b.key && a.codepoint == b.codepoint && a.mods == b.mods;
}

// A parsed binding: "<leader>ff" is three chords.
struct KeyChordSequence {
  KeyChord *chords;
  u64 count;
};

inline constexpr u64 kMaxChordSequence = 16;

// The default <leader>, matching vim's convention of a space.
[[nodiscard]] inline KeyChord KeyChordDefaultLeader() { return KeyChordChar(' '); }

// Parses one chord starting at `*offset`, advancing it past what was consumed.
// Understands `<C-x>` `<S-x>` `<A-x>` `<D-x>` (combinable, e.g. `<C-S-x>`),
// the named keys `<Esc> <CR> <Tab> <Space> <BS> <Del> <Up> <Left> <F1>` ...,
// `<leader>`, `<lt>` for a literal '<', and any bare character.
// Returns an invalid chord when the spec is malformed.
[[nodiscard]] KeyChord KeyChordParse(String8 spec, u64 *offset, KeyChord leader);

// Parses a whole binding spec into a sequence.
[[nodiscard]] KeyChordSequence KeyChordParseSequence(Arena *arena, String8 spec,
                                                     KeyChord leader = KeyChordDefaultLeader());

// Round-trips back to spec syntax, for the pending-keys indicator and for
// listing bindings.
[[nodiscard]] String8 KeyChordToString(Arena *arena, KeyChord chord);
[[nodiscard]] String8 KeyChordSequenceToString(Arena *arena, KeyChordSequence seq);

[[nodiscard]] String8 KeyName(Key key);
[[nodiscard]] Key KeyFromName(String8 name);
