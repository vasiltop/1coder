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

}  // namespace

Theme ThemeDefault() {
  Theme theme = {};

  // A dark, low-contrast base. The point is that text is the only thing with
  // any real brightness, so chrome never competes with it for attention.
  theme.background = Rgb(0x11151C);
  theme.text = Rgb(0xC5CDD9);

  theme.cursor = Rgb(0xD7DAE0);
  theme.cursor_text = Rgb(0x11151C);
  theme.selection = Rgb(0x2C3A4D);
  theme.current_line = Rgb(0xFFFFFF, 0.025f);

  theme.status_background = Rgb(0x1B212B);
  theme.status_text = Rgb(0x9AA5B4);
  theme.status_background_inactive = Rgb(0x161A22);
  theme.status_text_inactive = Rgb(0x5A6472);

  theme.split_border = Rgb(0x000000);
  theme.message = Rgb(0x9AA5B4);
  theme.error = Rgb(0xE06C75);

  // Every kind gets a colour now, so tree-sitter can start emitting tokens
  // later without the renderer changing at all.
  theme.syntax[(u64)TokenKind::Default] = theme.text;
  theme.syntax[(u64)TokenKind::Keyword] = Rgb(0xC678DD);
  theme.syntax[(u64)TokenKind::Identifier] = theme.text;
  theme.syntax[(u64)TokenKind::Type] = Rgb(0xE5C07B);
  theme.syntax[(u64)TokenKind::Function] = Rgb(0x61AFEF);
  theme.syntax[(u64)TokenKind::String] = Rgb(0x98C379);
  theme.syntax[(u64)TokenKind::Character] = Rgb(0x98C379);
  theme.syntax[(u64)TokenKind::Number] = Rgb(0xD19A66);
  theme.syntax[(u64)TokenKind::Comment] = Rgb(0x5C6370);
  theme.syntax[(u64)TokenKind::Operator] = Rgb(0x56B6C2);
  theme.syntax[(u64)TokenKind::Punctuation] = Rgb(0x8A93A1);
  theme.syntax[(u64)TokenKind::Preprocessor] = Rgb(0xC678DD);
  theme.syntax[(u64)TokenKind::Constant] = Rgb(0xD19A66);
  theme.syntax[(u64)TokenKind::Error] = theme.error;

  return theme;
}
