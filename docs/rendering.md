# Fonts, colours and the gutter

Fonts are chosen at runtime. Colours default to a compiled palette and can be
overridden in config.

## Line numbers

The gutter shows each line's distance from the cursor, with `0` on the cursor's
own line — vim's `relativenumber`. `:number` switches to absolute numbers,
`:relativenumber` back, and `:nonumber` hides the gutter entirely.

To change what it starts as, edit one constant in `core/editor/editor.h`:

```cpp
inline constexpr LineNumberMode kLineNumberModeDefault = LineNumberMode::Relative;
```

The gutter sizes itself to the buffer's line count with a three-digit floor
(`kLineNumberMinDigits`), so it grows for a long file but never jitters as the
cursor moves. Each split counts from its own cursor, since the numbers are a
property of the view rather than the buffer.

## Fonts

Selection prefers Iosevka, then the usual programming faces, then whatever the
platform ships — Menlo on macOS, Consolas on Windows, DejaVu or Liberation on
Linux. Override in `~/.config/1coder/config.toml`:

```toml
[font]
path = "/path/to/font.ttf"
face = ""
size = 16.0
```

Environment variables still win over the file:

| | |
|---|---|
| `EDITOR_FONT` | path to a `.ttf` or `.ttc` |
| `EDITOR_FONT_FACE` | which face to take out of a `.ttc` collection |
| `EDITOR_FONT_SIZE` | starting size in points |

```sh
EDITOR_FONT_SIZE=16 1coder file.c
```

Font files are memory-mapped rather than read, which matters more than it
sounds: Iosevka ships as a single 421 MB collection of 162 faces, and mapping it
means only the tables and outlines actually used become resident. Faces are
matched by exact family name — `stbtt_FindMatchingFont` matches loosely enough
to hand back "Iosevka Fixed Thin" when asked for "Iosevka Fixed".

`stb_truetype` is vendored in `third_party/`; there is no font library
dependency.

## Colours

Defaults live in `app/render/theme.cpp`. Override any field via `[theme]` /
`[theme.syntax]` in the config file (see [configuration](config.md)). Reload
with `:config-reload`.

The palette values were taken from the author's `~/.config/i3/config`: `$bg`,
`$text`, and the focused/unfocused window colours. The last two are reused for
the panel status bars and the split divider, so which split has focus reads the
same way it does in the window manager. Syntax colours are not from that palette
and are set separately in the same file.

## Syntax highlighting

### Built-in languages

Eight languages ship with full keyword tables: C/C++, JavaScript, TypeScript,
Python, Rust, Go, TOML, and JSON. Language selection is based on the file
extension (case-insensitive). Files with an unknown or absent extension fall
back to the generic "Plain" lexer.

The fallback lexer still recognises numbers, brackets and punctuation,
operators, double- and single-quoted strings, both `//` and `#` line comments,
and a conservative set of shared keywords (`if`, `else`, `for`, `while`,
`return`, `break`, `continue`) plus common constants (`true`, `false`, `null`,
`nil`). Generated UI buffers — command window, file explorer, search results,
and similar non-file kinds — are never attached to a syntax cache and remain
unhighlighted.

### Definitions and lexer

All language definitions are compile-time data in `core/text/syntax_languages.cpp`:
plain C arrays of string literals and keyword groups pointing into the
read-only segment, with no heap allocation and no runtime configuration. There
is no Tree-sitter dependency.

The scanner in `core/text/syntax.cpp` walks the buffer through `BufferByteAt`
without copying the text. It handles:

- **Stateful block comments** — `/* … */` (C/C++, JS/TS, Rust, Go); the
  open/close delimiters are per-language and tracked across lines.
- **Python triple-quoted strings** — both `'''…'''` and `"""…"""`.
- **Backtick strings** — JS, TS, and Go template/raw strings.

Per-line incoming and outgoing `SyntaxMode` values are cached in a
`SyntaxLineCache` array so any line can be re-lexed independently.

### Incremental updates

After every `BufferReplace`, undo, or redo, `SyntaxEndEdit` re-lexes from the
first changed line and walks forward until the outgoing state of a rescanned
line matches what the cache previously recorded — meaning subsequent lines are
unaffected. A single-line edit in a large file typically rescans only a handful
of lines.

### Intentional limits

The lexer is lexical only: no AST semantics, no symbol resolution. It does not
handle JSX structure, template-literal interpolation, C++ or Rust raw-string
variants (`R"…"` / `r#"…"#`), or nested block-comment depth. These are
acceptable omissions for a minimal editor.
