#include "render/theme.h"

namespace {

[[nodiscard]] constexpr Vec4F32 Rgb(u32 hex, f32 alpha = 1.0f) {
  return Vec4F32{
      (f32)((hex >> 16) & 0xFF) / 255.0f,
      (f32)((hex >> 8) & 0xFF) / 255.0f,
      (f32)(hex & 0xFF) / 255.0f,
      alpha,
  };
}

// Taken from ~/.config/i3/config, so the editor sits inside the window manager
// rather than beside it.
constexpr u32 kBg = 0x121212;             // $bg
constexpr u32 kText = 0xFAFBF6;           // $text
constexpr u32 kFocused = 0x5B768D;        // $current_window
constexpr u32 kUnfocused = 0x46425E;      // $other_window

}  // namespace

Theme ThemeDefault() {
  Theme theme = {};

  theme.background = Rgb(kBg);
  theme.text = Rgb(kText);

  theme.cursor = Rgb(kText);
  theme.cursor_text = Rgb(kBg);
  theme.cursor_secondary = Rgb(kText, 0.55f);
  theme.cursor_pending = Rgb(kFocused);
  // The unfocused window colour doubles as the selection: it reads clearly
  // against the background without being as loud as the focused blue.
  theme.selection = Rgb(kUnfocused);
  theme.current_line = Rgb(0xFFFFFF, 0.03f);

  theme.line_number = Rgb(kText, 0.35f);
  theme.line_number_current = Rgb(kText, 0.9f);
  // Search matches use the focused-window blue so they stand apart from a
  // visual selection: with both on screen it must be obvious which is which.
  theme.search_match = Rgb(kFocused);

  // Panel status bars mirror i3's window borders, so which window has focus
  // reads the same way here as it does on the desktop.
  theme.status_background = Rgb(kFocused);
  theme.status_text = Rgb(kText);
  theme.status_background_inactive = Rgb(kUnfocused);
  theme.status_text_inactive = Rgb(kText, 0.65f);

  // i3 separates adjacent windows with a border in this colour, and with the
  // background now matching $bg a divider is the only thing distinguishing two
  // panels' text areas from one another.
  theme.split_border = Rgb(kUnfocused);
  theme.message = Rgb(kText, 0.75f);
  theme.error = Rgb(0xE06C75);  // the palette has no red of its own
  theme.diagnostic_error = theme.error;
  theme.diagnostic_warning = Rgb(0xE5C07B);
  theme.diagnostic_information = Rgb(0x61AFEF);
  theme.diagnostic_hint = Rgb(0x98C379);
  theme.popup_background = Rgb(0x1B1D23);
  theme.popup_border = theme.split_border;
  theme.popup_selected = Rgb(kFocused, 0.85f);
  theme.popup_text = theme.text;
  theme.popup_detail = theme.message;

  // Syntax colours are not in the i3 palette, so these stay as they were. Only
  // Default is tied to the scheme, which keeps ordinary text consistent.
  theme.syntax[(u64)TokenKind::Default] = theme.text;
  theme.syntax[(u64)TokenKind::Keyword] = Rgb(0xC678DD);
  theme.syntax[(u64)TokenKind::Identifier] = theme.text;
  theme.syntax[(u64)TokenKind::Type] = Rgb(0xE5C07B);
  theme.syntax[(u64)TokenKind::Function] = Rgb(0x61AFEF);
  theme.syntax[(u64)TokenKind::String] = Rgb(0x98C379);
  theme.syntax[(u64)TokenKind::Character] = Rgb(0x98C379);
  theme.syntax[(u64)TokenKind::Number] = Rgb(0xD19A66);
  theme.syntax[(u64)TokenKind::Comment] = Rgb(0x6B7280);
  theme.syntax[(u64)TokenKind::Operator] = Rgb(0x56B6C2);
  theme.syntax[(u64)TokenKind::Punctuation] = Rgb(kText, 0.7f);
  theme.syntax[(u64)TokenKind::Preprocessor] = Rgb(0xC678DD);
  theme.syntax[(u64)TokenKind::Constant] = Rgb(0xD19A66);
  theme.syntax[(u64)TokenKind::Error] = theme.error;

  return theme;
}

namespace {

[[nodiscard]] i32 HexNibble(u8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

[[nodiscard]] bool ParseHexColor(String8 hex, Vec4F32 *out) {
  if (!out || hex.size == 0) return false;
  String8 s = hex;
  if (s.str[0] == '#') s = Str8Skip(s, 1);
  if (s.size != 6 && s.size != 8) return false;

  u32 value = 0;
  for (u64 i = 0; i < s.size; i += 1) {
    i32 n = HexNibble(s.str[i]);
    if (n < 0) return false;
    value = (value << 4) | (u32)n;
  }

  if (s.size == 6) {
    *out = Rgb(value);
  } else {
    u32 rgb = value >> 8;
    f32 alpha = (f32)(value & 0xFF) / 255.0f;
    *out = Rgb(rgb, alpha);
  }
  return true;
}

[[nodiscard]] bool TokenKindFromName(String8 name, TokenKind *out) {
  struct Entry {
    const char *name;
    TokenKind kind;
  };
  static const Entry kKinds[] = {
      {"default", TokenKind::Default},
      {"keyword", TokenKind::Keyword},
      {"identifier", TokenKind::Identifier},
      {"type", TokenKind::Type},
      {"function", TokenKind::Function},
      {"string", TokenKind::String},
      {"character", TokenKind::Character},
      {"number", TokenKind::Number},
      {"comment", TokenKind::Comment},
      {"operator", TokenKind::Operator},
      {"punctuation", TokenKind::Punctuation},
      {"preprocessor", TokenKind::Preprocessor},
      {"constant", TokenKind::Constant},
      {"error", TokenKind::Error},
  };
  for (u64 i = 0; i < ArrayCount(kKinds); i += 1) {
    if (Str8Match(name, Str8C(kKinds[i].name), StringMatch::CaseInsensitive)) {
      *out = kKinds[i].kind;
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool ApplyNamedColor(Theme *theme, String8 name, Vec4F32 color) {
  if (Str8StartsWith(name, Str8Lit("syntax."))) {
    TokenKind kind = {};
    if (!TokenKindFromName(Str8Skip(name, 7), &kind)) return false;
    theme->syntax[(u64)kind] = color;
    return true;
  }

  struct Field {
    const char *name;
    Vec4F32 Theme::*member;
  };
  static const Field kFields[] = {
      {"background", &Theme::background},
      {"text", &Theme::text},
      {"cursor", &Theme::cursor},
      {"cursor_text", &Theme::cursor_text},
      {"cursor_secondary", &Theme::cursor_secondary},
      {"cursor_pending", &Theme::cursor_pending},
      {"selection", &Theme::selection},
      {"search_match", &Theme::search_match},
      {"current_line", &Theme::current_line},
      {"line_number", &Theme::line_number},
      {"line_number_current", &Theme::line_number_current},
      {"status_background", &Theme::status_background},
      {"status_text", &Theme::status_text},
      {"status_background_inactive", &Theme::status_background_inactive},
      {"status_text_inactive", &Theme::status_text_inactive},
      {"split_border", &Theme::split_border},
      {"message", &Theme::message},
      {"error", &Theme::error},
      {"diagnostic_error", &Theme::diagnostic_error},
      {"diagnostic_warning", &Theme::diagnostic_warning},
      {"diagnostic_information", &Theme::diagnostic_information},
      {"diagnostic_hint", &Theme::diagnostic_hint},
      {"popup_background", &Theme::popup_background},
      {"popup_border", &Theme::popup_border},
      {"popup_selected", &Theme::popup_selected},
      {"popup_text", &Theme::popup_text},
      {"popup_detail", &Theme::popup_detail},
  };
  for (u64 i = 0; i < ArrayCount(kFields); i += 1) {
    if (Str8Match(name, Str8C(kFields[i].name), StringMatch::CaseInsensitive)) {
      theme->*(kFields[i].member) = color;
      return true;
    }
  }
  return false;
}

}  // namespace

Theme ThemeFromConfig(const Config *config) {
  Theme theme = ThemeDefault();
  if (!config) return theme;
  for (u64 i = 0; i < config->color_count; i += 1) {
    Vec4F32 color = {};
    if (!ParseHexColor(config->colors[i].hex, &color)) continue;
    (void)ApplyNamedColor(&theme, config->colors[i].name, color);
  }
  return theme;
}
