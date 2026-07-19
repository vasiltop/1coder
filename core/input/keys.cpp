#include "input/keys.h"

namespace {

struct KeyNameEntry {
  const char *name;
  Key key;
};

// Names accepted inside <> and produced by KeyChordToString. The first entry
// for a key is the canonical spelling; later duplicates are accepted aliases.
constexpr KeyNameEntry kKeyNames[] = {
    {"Esc", Key::Escape},   {"Escape", Key::Escape},
    {"CR", Key::Return},    {"Return", Key::Return},   {"Enter", Key::Return},
    {"Tab", Key::Tab},      {"Space", Key::Space},
    {"BS", Key::Backspace}, {"Backspace", Key::Backspace},
    {"Del", Key::Delete},   {"Delete", Key::Delete},
    {"Insert", Key::Insert},
    {"Left", Key::Left},    {"Right", Key::Right},
    {"Up", Key::Up},        {"Down", Key::Down},
    {"Home", Key::Home},    {"End", Key::End},
    {"PageUp", Key::PageUp},{"PageDown", Key::PageDown},
    {"F1", Key::F1},   {"F2", Key::F2},   {"F3", Key::F3},   {"F4", Key::F4},
    {"F5", Key::F5},   {"F6", Key::F6},   {"F7", Key::F7},   {"F8", Key::F8},
    {"F9", Key::F9},   {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12},
};

// Maps a bare character to the key that produces it, for `<C-w>` style specs
// where the spec names a character but the chord must be key-based.
[[nodiscard]] Key KeyFromChar(u32 codepoint) {
  if (codepoint >= 'a' && codepoint <= 'z') {
    return (Key)((u16)Key::A + (codepoint - 'a'));
  }
  if (codepoint >= 'A' && codepoint <= 'Z') {
    return (Key)((u16)Key::A + (codepoint - 'A'));
  }
  if (codepoint >= '0' && codepoint <= '9') {
    return (Key)((u16)Key::Num0 + (codepoint - '0'));
  }
  switch (codepoint) {
    case ' ':  return Key::Space;
    case '-':  return Key::Minus;
    case '=':  return Key::Equal;
    case '[':  return Key::LeftBracket;
    case ']':  return Key::RightBracket;
    case '\\': return Key::Backslash;
    case ';':  return Key::Semicolon;
    case '\'': return Key::Apostrophe;
    case '`':  return Key::Grave;
    case ',':  return Key::Comma;
    case '.':  return Key::Period;
    case '/':  return Key::Slash;
    default:   return Key::Unknown;
  }
}

// Parses the body of a <...> form, which has already been isolated.
[[nodiscard]] KeyChord ParseAngleForm(String8 body, KeyChord leader) {
  if (body.size == 0) return KeyChord{};

  if (Str8Match(body, Str8Lit("leader"), StringMatch::CaseInsensitive)) return leader;
  if (Str8Match(body, Str8Lit("lt"), StringMatch::CaseInsensitive)) return KeyChordChar('<');
  if (Str8Match(body, Str8Lit("gt"), StringMatch::CaseInsensitive)) return KeyChordChar('>');

  // Strip any number of modifier prefixes: "C-", "S-", "A-", "M-", "D-".
  KeyMod mods = KeyMod::None;
  while (body.size >= 2 && body.str[1] == '-') {
    switch (CharToUpper(body.str[0])) {
      case 'C': mods |= KeyMod::Ctrl; break;
      case 'S': mods |= KeyMod::Shift; break;
      case 'A': mods |= KeyMod::Alt; break;
      case 'M': mods |= KeyMod::Alt; break;
      case 'D': mods |= KeyMod::Super; break;
      default: return KeyChord{};
    }
    body = Str8Skip(body, 2);
  }

  if (body.size == 0) return KeyChord{};

  // A named key, with or without modifiers.
  Key named = KeyFromName(body);
  if (named != Key::Unknown) return KeyChordKey(named, mods);

  // Otherwise it must be a single character.
  DecodedCodepoint d = Utf8Decode(body, 0);
  if (d.advance != body.size) return KeyChord{};

  // With no modifiers a <x> form is just the character itself. With modifiers
  // it becomes a key chord, because the character a modified press would
  // produce is not what the binding means.
  if (mods == KeyMod::None) return KeyChordChar(d.codepoint);

  Key key = KeyFromChar(d.codepoint);
  if (key == Key::Unknown) return KeyChord{};

  // "<C-S-a>" and "<C-A>" mean the same press, so fold an uppercase letter
  // into an explicit Shift rather than leaving the two forms distinct.
  if (CharIsUpper((u8)d.codepoint)) mods |= KeyMod::Shift;

  return KeyChordKey(key, mods);
}

}  // namespace

String8 KeyName(Key key) {
  for (u64 i = 0; i < ArrayCount(kKeyNames); i += 1) {
    if (kKeyNames[i].key == key) return Str8C(kKeyNames[i].name);
  }
  return String8{nullptr, 0};
}

Key KeyFromName(String8 name) {
  for (u64 i = 0; i < ArrayCount(kKeyNames); i += 1) {
    if (Str8Match(name, Str8C(kKeyNames[i].name), StringMatch::CaseInsensitive)) {
      return kKeyNames[i].key;
    }
  }
  return Key::Unknown;
}

KeyChord KeyChordParse(String8 spec, u64 *offset, KeyChord leader) {
  if (*offset >= spec.size) return KeyChord{};

  if (spec.str[*offset] == '<') {
    u64 close = Str8FindFirstChar(spec, '>', *offset + 1);
    if (close < spec.size) {
      String8 body = Str8Substr(spec, RangeU64{*offset + 1, close});
      KeyChord chord = ParseAngleForm(body, leader);
      if (KeyChordValid(chord)) {
        *offset = close + 1;
        return chord;
      }
    }
    // An unmatched or unrecognised '<' is taken literally, so a binding on the
    // '<' key itself does not need escaping.
  }

  DecodedCodepoint d = Utf8Decode(spec, *offset);
  *offset += d.advance;
  return KeyChordChar(d.codepoint);
}

KeyChordSequence KeyChordParseSequence(Arena *arena, String8 spec, KeyChord leader) {
  KeyChordSequence seq = {};
  if (spec.size == 0) return seq;

  KeyChord *chords = PushArray(arena, KeyChord, kMaxChordSequence);
  u64 count = 0;
  u64 offset = 0;

  while (offset < spec.size && count < kMaxChordSequence) {
    KeyChord chord = KeyChordParse(spec, &offset, leader);
    if (!KeyChordValid(chord)) return KeyChordSequence{};  // malformed spec
    chords[count] = chord;
    count += 1;
  }

  // Reject rather than truncate: a silently shortened spec would install a
  // binding on the wrong prefix.
  if (offset < spec.size) return KeyChordSequence{};

  seq.chords = chords;
  seq.count = count;
  return seq;
}

String8 KeyChordToString(Arena *arena, KeyChord chord) {
  if (!KeyChordValid(chord)) return String8{nullptr, 0};

  // A plain character prints as itself, except '<' which must be escaped so the
  // output can be parsed back.
  if (KeyChordIsChar(chord) && chord.mods == KeyMod::None) {
    if (chord.codepoint == '<') return PushStr8Copy(arena, Str8Lit("<lt>"));
    u8 buffer[4];
    u32 n = Utf8Encode(buffer, chord.codepoint);
    return PushStr8Copy(arena, String8{buffer, n});
  }

  TempArena scratch = ScratchBegin1(arena);
  String8List parts = {};

  if (HasFlag(chord.mods, KeyMod::Ctrl)) Str8ListPush(scratch.arena, &parts, Str8Lit("C-"));
  if (HasFlag(chord.mods, KeyMod::Shift)) Str8ListPush(scratch.arena, &parts, Str8Lit("S-"));
  if (HasFlag(chord.mods, KeyMod::Alt)) Str8ListPush(scratch.arena, &parts, Str8Lit("A-"));
  if (HasFlag(chord.mods, KeyMod::Super)) Str8ListPush(scratch.arena, &parts, Str8Lit("D-"));

  if (KeyChordIsChar(chord)) {
    u8 buffer[4];
    u32 n = Utf8Encode(buffer, chord.codepoint);
    Str8ListPush(scratch.arena, &parts, PushStr8Copy(scratch.arena, String8{buffer, n}));
  } else {
    String8 name = KeyName(chord.key);
    if (name.size == 0) {
      // Unnamed keys are letters, digits and punctuation; print them lowercase
      // so "<C-w>" round-trips.
      u16 v = (u16)chord.key;
      u8 c = 0;
      if (chord.key >= Key::A && chord.key <= Key::Z) {
        c = (u8)('a' + (v - (u16)Key::A));
      } else if (chord.key >= Key::Num0 && chord.key <= Key::Num9) {
        c = (u8)('0' + (v - (u16)Key::Num0));
      } else {
        switch (chord.key) {
          case Key::Minus: c = '-'; break;
          case Key::Equal: c = '='; break;
          case Key::LeftBracket: c = '['; break;
          case Key::RightBracket: c = ']'; break;
          case Key::Backslash: c = '\\'; break;
          case Key::Semicolon: c = ';'; break;
          case Key::Apostrophe: c = '\''; break;
          case Key::Grave: c = '`'; break;
          case Key::Comma: c = ','; break;
          case Key::Period: c = '.'; break;
          case Key::Slash: c = '/'; break;
          default: c = '?'; break;
        }
      }
      name = PushStr8Copy(scratch.arena, String8{&c, 1});
    }
    Str8ListPush(scratch.arena, &parts, name);
  }

  String8 body = Str8ListJoin(scratch.arena, &parts, String8{nullptr, 0});
  String8 result = PushStr8F(arena, "<%.*s>", (int)body.size, (char *)body.str);
  ScratchEnd(scratch);
  return result;
}

String8 KeyChordSequenceToString(Arena *arena, KeyChordSequence seq) {
  TempArena scratch = ScratchBegin1(arena);
  String8List parts = {};
  for (u64 i = 0; i < seq.count; i += 1) {
    Str8ListPush(scratch.arena, &parts, KeyChordToString(scratch.arena, seq.chords[i]));
  }
  String8 result = Str8ListJoin(arena, &parts, String8{nullptr, 0});
  ScratchEnd(scratch);
  return result;
}
