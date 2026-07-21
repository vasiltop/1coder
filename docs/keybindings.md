# Keybindings

Defaults are defined in `core/vim/vim_binds.cpp`, mirroring `~/.config/nvim` for
every feature that exists here. Leader is space; `shiftwidth` is 2 and
`scrolloff` is 4, matching that config.

Override any binding in `~/.config/1coder/config.toml` under `[bindings.*]`
(see [configuration](config.md) and `config/config.example.toml`). 
`:config-reload` re-applies the file; `:bindings` lists everything the running
editor knows.

## Motions and operators

Motions `h j k l w b e W B E 0 ^ _ $ gg G { } f F t T %` · operators `d c y > <`
composed with any motion, plus `dd yy cc >> <<` · edits `x X D C p P J u <C-r> .`
· insert `i I a A o O R` · visual `v V` · scrolling `<C-d> <C-u> <C-e> <C-y> zz`
· jump list `<C-o>` / `<C-i>` (and `<Tab>` for newer).

Text objects: `iw` `aw` `i"` `a"` `i(` `a(` `i{` `a{` `i[` `a[` `ip` `ap`, with
any operator (`ciw`, `da(`) and in visual mode (`vi{`).

## Windows

`<C-h/j/k/l>` moves focus, `<leader>v` splits vertically and `<leader>h`
horizontally (as `:vsplit` and `:split` do). The built-in `<C-w>` forms —
`<C-w>v` `<C-w>s` `<C-w>hjkl` `<C-w>c` `<C-w>o` — work too. `<C-f>` is the
same as `<C-w>o`: close every other window.

## Mouse

Mouse support is always on for the supported editor surfaces; there is not yet
a config toggle for it.

- **Buffer text:** left click focuses the pane and places the cursor; drag makes
  a charwise selection; double click selects the word or matching-bracket
  range; triple click selects the line; right click extends the nearest
  selection edge, or places the cursor if nothing is selected.
- **Ctrl + left click:** adds a cursor where it lands, or removes the one
  already there — the last cursor is never removed. Clicking off the end of a
  line adds a cursor past its last character, as `A` does while placing. Dragging
  from the new cursor pulls out a selection under it while the others stay put. A
  plain click collapses back to one cursor. See
  [multiple cursors](#multiple-cursors).
- **Panel gutter:** click goes to column 0 of that line; drag starts a
  charwise selection from line start.
- **Command line:** click places the command-line cursor; drag selects there;
  middle click pastes into the command line.
- **Middle click:** pastes clipboard text in Normal, Visual, Insert, and
  Replace; in Visual it replaces the selection.
- **Wheel:** scrolls the pane under the pointer without changing focus;
  vertical wheel scrolls vertically, horizontal wheel scrolls horizontally, and
  `Shift`+vertical or horizontal wheel pages instead of line-scrolling.
- **Splits and status lines:** drag a vertical split edge to resize left/right
  panes; drag an eligible panel status-line boundary to resize top/bottom
  panes; clicking a status line focuses that pane.

Tag/search modifier gestures and `Alt`-right block selection are not supported.

## Registers

Registers work as in vim: `"a` picks one for the next yank, delete or paste,
`""` is the unnamed register, `"0` holds the last yank and survives deletes, and
`"+` / `"*` are the system clipboard — so `"+y` and `"+p` exchange text with
other programs while a bare `y`/`p` stays internal. A named yank fills the
unnamed register too.

The clipboard carries no notion of charwise versus linewise, so pasting infers
one. Text the editor put there keeps the kind it was captured with; text from
another program is linewise only if it really spans lines, and a lone trailing
newline — what selecting a line in a browser produces — is dropped rather than
splitting the line it is pasted into.

## Macros

`q{reg}` records, `q` stops, `@{reg}` replays, `@@` repeats. A macro is stored as
a binding spec in a register, so it can be pasted, edited and yanked back like
any other text.

## Insert mode

`<C-w>`, `<C-h>` and `<C-BS>` all rub out the previous word; `<C-r>{reg}`
inserts a register.

## Zoom

`<C-=>` / `<C-->` / `<C-0>`, in any mode. The core holds only a font size; the
app watches it and rebuilds the glyph atlas, and because layout is in cells the
grid resizes through the same path a window resize takes.

## Search and files

`<leader>pg` searches the project as you type, `<leader>pf` fuzzy-finds a file,
`<leader>pb` fuzzy-picks an open buffer, `<leader>pp` fuzzy-finds a git
repository root (sets the working directory and opens the explorer on select),
`<leader>sc` sets the working directory from the current file or explorer entry,
and `<leader>e` opens the file explorer for the current file's directory. See
[search](search.md) and [explorer](explorer.md).

## Git

`<leader>gs` or `:git` opens a Magit-style status buffer. In that buffer `s`/`u`
stage and unstage, `Tab` expands hunks, `F`/`P` pull and push, and `r` toggles
`--rebase` for the next pull (shown on the `Args:` line). See [git](git.md).

## Language server protocol

`gi` jumps to implementation, `gd` to definition, `gD` to declaration, and `gt`
to type definition. `<leader>cf` formats, `<leader>rn` renames, and `<leader>d`
shows diagnostics at the cursor. In normal mode `<C-Space>` hovers; in insert
mode it completes (cycle with Up/Down or Ctrl-N/Ctrl-P, accept with Enter,
dismiss with Esc). See [language server protocol](lsp.md).

## Compile

`:compile <command>` runs any shell command and streams its output into a
`[compile]` buffer. If that buffer is already visible in some window, that
window is reused; otherwise it opens in a vertical split beside the current
one (`:vsplit` / `<leader>v`). With no argument it opens the command window
prefilled with the last command, or `make -k` if none has been run yet.
`:recompile` and `<leader>rc` rerun the last command.

GNU-style diagnostics (`file:line:col:` / `file:line:`) are collected from the
output. `<leader>ne` and `<leader>pe` jump to the next and previous error in a
circle; `<CR>` on an error line in the compile buffer opens that locus.

## Multiple cursors

`<leader>mc` starts placing cursors and marks the position you are on. Every
motion still works while placing, so you aim with `j`, `w`, `/foo` or anything
else; `c` marks the position you land on, and marking one twice takes it back.
`A` marks the end of the current line — normal mode cannot rest past the last
character, so this is the only way to put a cursor there. `<CR>` makes the marks
live, `<Esc>` abandons them.

Placement is staged rather than instant so that the whole motion vocabulary is
available for choosing positions, instead of a handful of add-a-cursor keys.

Ctrl + left click adds and removes cursors directly, without placement — the
faster route when the positions are already on screen.

Once cursors are live, everything fans out: `x`, `dw`, `ciwfoo<Esc>` and plain
typing all happen at every cursor, and the whole pass is one undo step. `v` and
`V` give each cursor its own selection, so `vlld` or `vac` acts at all of them;
ctrl-drag pulls out a selection under a freshly added cursor. Cursors that meet
merge into one, as do selections that grow into each other. `<Esc>` in normal
mode drops back to a single cursor — so from insert mode it takes two, one to
leave insert and one to collapse.

Commands that are properties of the window rather than of a cursor — `u`, `.`,
macros, `/`, splits, scrolling — run once however many cursors are live.

## The command window

Opens in insert mode, as `q:i` does in vim. `<Esc>` drops to normal mode where
every motion and operator applies to the line being typed; `<Esc>` again
abandons it. `<CR>` submits from any mode. Syntax is in [commands](commands.md).
