# Architecture

The load-bearing decision is the line between `core/` and `app/`:

```
                    ┌─ tests/    links core only, no SDL, no display
core (static lib) ──┤
  base/     arena allocator, String8, UTF-8, ranges, cell rects
  os/       file and directory access, process pipes
  text/     gap buffer, line index, undo stack, syntax tokens
  input/    Key / KeyChord types, binding parser, keymap trie
  command/  command identity: the COMMAND_LIST table, name <-> id
  editor/   buffers, views, panels, dispatch, command line parsing
  search/   project walk, grep, fuzzy matching
  vim/      modes, motions, operators, default bindings
  lsp/      language server protocol client, diagnostics, completion
  buffers/  per-kind buffer implementations
                    └─ app (executable)
  platform/ SDL window, event pump, SDL_Keycode -> Key
  render/   glyph atlas, batched draw, painter, theme
```

`editor_core` links no graphics library at all, so a stray SDL include in
`core/` fails the build — `just boundary` checks it, and CI runs that check.
Everything about editing — motions, operators, undo, panel layout, scrolling,
command parsing — is testable without opening a window, and the test binary
needs no display.

Two things make that work:

- **Input is platform-agnostic.** `Key` is our own enum; `app/platform/` is the
  only file in the program that mentions `SDL_Keycode`. Tests synthesise chords
  directly.
- **The core lays out in cells, not pixels.** Panel rects, scroll positions and
  cursor columns are all integer cell arithmetic. The renderer is the only thing
  that knows a glyph's size.

## Design decisions

**Everything is a buffer.** Files, the command window, the file explorer, and
the `:commands` / `:buffers` / `:bindings` listings are all `Buffer`s differing
only in their kind, their hooks, and a `void *user_data` payload. A git client
would be a new file in `core/buffers/` providing a `BufferHooks` table — no core
changes.

**Vim bindings work in every buffer**, including the command window. A buffer
claims the few keys it needs through a buffer-local keymap, and that map layers
*above* whichever mode map is active rather than replacing it — the input layer
reparents it onto the current mode on each lookup. So the command window binds
`<CR>`, `<Tab>` and `<Esc>`, and everything else — `dw`, `ciw`, `b`, `$`, `u`,
counts, insert mode — is inherited. Nothing opts out of modal editing, and a
new pane gets it for free.

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

**Language server protocol is non-blocking.** LSP server startup and message
handling happen on a background thread, and the editor wakes the main thread
to redraw as results arrive. The `core/lsp` module has no window-system
dependencies, so LSP features are testable in the test binary. Process pipes
live in `core/os/`, and the editor thread drains incoming messages during its
event loop, keeping frame times predictable.

## The os layer

`core/os/os_file.h` is the only place that talks to the filesystem. Two
invariants matter to everything above it:

- **Every path handed back uses `/` as the separator, on Windows too.** Win32
  accepts forward slashes on input and only ever emits backslashes, so the
  conversion happens once in `OsGetCwd` and `OsPathAbsolute` rather than leaving
  the explorer, the search results and the status line to cope with two
  spellings of the same path. Paths coming *in* may use either.
- **A root has no parent.** `Str8PathIsRoot` is the single answer to that
  question — `/` on POSIX, `C:/` on Windows. Note that `C:` is *not* a root: it
  names the current directory on that drive, which is a different place.

Platform-specific code is confined to `core/os/os_file.cpp` and the `_WIN32`
branches in `core/base/base_arena.cpp` and `core/base/base_string.cpp`. Adding a
platform should not require touching anything else.
