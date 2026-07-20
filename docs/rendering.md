# Fonts and colours

Fonts are chosen at runtime. Colours are compiled in.

## Fonts

Selection prefers Iosevka, then the usual programming faces, then whatever the
platform ships — Menlo on macOS, Consolas on Windows, DejaVu or Liberation on
Linux. Override with environment variables:

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

Colours are compiled in — `app/render/theme.cpp`, about seventy lines of named
constants. Nothing is read at runtime, so changing the theme means editing that
file and rebuilding.

The palette values were taken from the author's `~/.config/i3/config`: `$bg`,
`$text`, and the focused/unfocused window colours. The last two are reused for
the panel status bars and the split divider, so which split has focus reads the
same way it does in the window manager. Syntax colours are not from that palette
and are set separately in the same file.
