# Keybindings

Defined in `core/vim/vim_binds.cpp`, mirroring `~/.config/nvim` for every
feature that exists here. Leader is space; `shiftwidth` is 2 and `scrolloff`
is 4, matching that config.

`:bindings` lists everything the running editor knows.

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
`<C-w>v` `<C-w>s` `<C-w>hjkl` `<C-w>c` `<C-w>o` — work too.

## Mouse

Mouse support is always on for the supported editor surfaces; there is no
config toggle.

- **Buffer text:** left click focuses the pane and places the cursor; drag makes
  a charwise selection; double click selects the word or matching-bracket
  range; triple click selects the line; right click extends the nearest
  selection edge, or places the cursor if nothing is selected.
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
`<leader>pb` fuzzy-picks an open buffer, and `<leader>e` opens the file explorer
for the current file's directory. See [search](search.md) and [explorer](explorer.md).

## Compile

`:compile <command>` runs any shell command and streams its output into a
`[compile]` buffer. If that buffer is already visible in some window, that
window is reused; otherwise it opens in a vertical split beside the current
one (`:vsplit` / `<leader>v`). With no argument it opens the command window
prefilled with the last command, or `make -k` if none has been run yet.
`:recompile` and `<leader>rc` rerun the last command.

## The command window

Opens in insert mode, as `q:i` does in vim. `<Esc>` drops to normal mode where
every motion and operator applies to the line being typed; `<Esc>` again
abandons it. `<CR>` submits from any mode. Syntax is in [commands](commands.md).
