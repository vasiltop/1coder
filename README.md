# 1code

A graphical modal text editor in procedural C++23. Handmade Hero / RAD Debugger
style: plain structs and free functions, arena allocation, no inheritance, no
scripting language. All configuration is source code.

## Build

Needs CMake, a C++23 compiler, and SDL3.

```sh
cmake -B build -S .
cmake --build build -j

./build/editor path/to/file      # run
./build/editor_tests             # 229 tests, needs no display
```

`stb_truetype` is vendored in `third_party/`. Nothing else is required.

Set `EDITOR_FONT` to a `.ttf` path and `EDITOR_FONT_SIZE` to override font
selection.

## Architecture

The load-bearing decision is the line between `core/` and `app/`:

```
                    ┌─ tests/    links core only, no SDL, no display
core (static lib) ──┤
  base/     arena allocator, String8, UTF-8, ranges, cell rects
  os/       file and directory access
  text/     gap buffer, line index, undo stack, syntax tokens
  input/    Key / KeyChord types, binding parser, keymap trie
  command/  command identity: the COMMAND_LIST table, name <-> id
  editor/   buffers, views, panels, dispatch, command line parsing
  vim/      modes, motions, operators, default bindings
  buffers/  per-kind buffer implementations
                    └─ app (executable)
  platform/ SDL window, event pump, SDL_Keycode -> Key
  render/   glyph atlas, batched draw, painter, theme
```

`editor_core` links no graphics library at all, so a stray SDL include in
`core/` fails the build. Everything about editing — motions, operators, undo,
panel layout, scrolling, command parsing — is testable without opening a window,
and the test binary needs no display.

Two things make that work:

- **Input is platform-agnostic.** `Key` is our own enum; `app/platform/` is the
  only file in the program that mentions `SDL_Keycode`. Tests synthesise chords
  directly.
- **The core lays out in cells, not pixels.** Panel rects, scroll positions and
  cursor columns are all integer cell arithmetic. The renderer is the only thing
  that knows a glyph's size.

## Design notes

**Everything is a buffer.** Files, the command window, and the `:commands` /
`:buffers` / `:bindings` listings are all `Buffer`s differing only in their kind,
their hooks, and a `void *user_data` payload. A file explorer or git client is a
new file in `core/buffers/` providing a `BufferHooks` table — no core changes.

**One mutation path.** All text changes go through `BufferReplace`, which
updates the gap buffer, patches the line index, pushes undo, sets the dirty flag
and fires hooks. Consistency is structural rather than remembered, and read-only
enforcement needs exactly one check.

**Motions and operators are orthogonal.** A motion is a pure function returning a
target offset and a kind (exclusive / inclusive / linewise). An operator consumes
a range. So `d` composes with every motion, and a new motion works with every
operator, without any per-pair code. `dd` is not a special case: `d` is bound in
the normal map, and `d` again in the operator-pending map.

**One dispatch path.** Keybindings and the command window both resolve to a
`CommandId` and run the same body, so `:1d` and `dd` cannot drift apart.

## Keybindings

Defined in `core/vim/vim_binds.cpp`, mirroring `~/.config/nvim` for every
feature that exists here. Leader is space; `shiftwidth` is 2 and `scrolloff`
is 4, matching that config.

Motions `h j k l w b e W B E 0 ^ $ gg G { } f F t T %` · operators `d c y > <`
composed with any motion, plus `dd yy cc >> <<` · edits `x X D C p P J u <C-r> .`
· insert `i I a A o O R` · visual `v V` · scrolling `<C-d> <C-u> <C-e> <C-y> zz`.

Windows: `<C-h/j/k/l>` moves focus, `<leader>v` splits vertically and
`<leader>h` horizontally (as `:vsplit` and `:split` do). The built-in `<C-w>`
forms — `<C-w>v` `<C-w>s` `<C-w>hjkl` `<C-w>c` `<C-w>o` — work too.

Insert mode: `<C-w>`, `<C-h>` and `<C-BS>` all rub out the previous word.

## Command window

Opened with `:`. Syntax:

```
[range] name[!] [arguments]
```

- **range** — `%` whole buffer · `.` current line · `$` last line · `N` ·
  `N,M` · `.,+3` · `'<,'>` the visual selection
- **name** — a full name, an unambiguous prefix (`und` → `undo`), or a short
  alias (`w` `q` `wq` `qa` `e` `vs` `sp` `bn` …)
- **!** — the forced variant, where a command defines one (`q!` discards unsaved
  changes)
- **arguments** — whitespace separated; `"quote them"` to include spaces

Tab completes: command names while typing the name, then whatever the command's
declared argument kind calls for — filesystem paths, buffer names, or command
names. `:commands` lists everything with its shape.

### Adding a command

One line in `COMMAND_LIST` (`core/command/command_id.h`) declaring its name,
description, argument kind and flags:

```c
X(reload_file, "reload", "Reread the file from disk", CommandArg::None, CmdBang)
```

and a body in `core/editor/command.cpp`:

```cpp
static void Cmd_reload_file(CommandArgs *a) { /* ... */ }
```

The macro generates the enum, the name/id hash and the dispatch table. A missing
body is a link error, not a silent gap. It is immediately reachable from the
command window, from completion, and from any keybinding.

## Development

`--screenshot <path>` renders one frame to a BMP and exits; `--keys <spec>`
replays a binding spec first. Together they capture the editor in any state
without a display:

```sh
SDL_VIDEODRIVER=dummy ./build/editor file.c --keys '<C-w>v10jVj' --screenshot out.bmp
```

## Not yet implemented

Tree-sitter highlighting (the `TokenArray` seam and the theme's colour table are
in place and unused), project-wide grep, fuzzy file finding, a file explorer, a
git client, macros, marks, visual block mode, and a Windows backend for
`os_file.cpp`.
