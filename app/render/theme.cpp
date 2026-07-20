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
